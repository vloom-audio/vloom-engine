// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/dsp/DelayLine.hpp"
#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace vloom {

/// Longest latency the engine compensates (ADR 0007): about 1.4 s at
/// 48 kHz. More is counted as this much.
inline constexpr int kMaxLatencyFrames = 65536;

/// A hosted processor as the engine holds it (ADR 0006): the processor plus
/// activation, bypass and a queue carrying parameter changes from the
/// control thread to the audio thread. Shared (std::shared_ptr) by every
/// mixer snapshot holding it, and destroyed on the control thread.
///
/// Control thread: activate(), deactivate(), setParameter(), setBypassed(),
/// processor(). Audio thread: process(), reset().
class PluginInstance {
public:
    /// Parameter changes that fit in the queue between two blocks; more are
    /// refused (setParameter() returns false) until the audio thread drains it.
    static constexpr std::size_t kMaxQueuedParameters = 255;

    explicit PluginInstance(std::unique_ptr<AudioProcessor> processor);
    ~PluginInstance();
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;

    /// Activates the processor for `sampleRate` and blocks of at most
    /// `maxBlockFrames` (deactivating it first if it was active). Not while
    /// the audio thread may process it: before it is published, or from
    /// Engine::prepare(). False if the processor refused; it then passes
    /// audio through.
    bool activate(double sampleRate, int maxBlockFrames);
    void deactivate();
    [[nodiscard]] bool isActive() const noexcept { return active_.load(std::memory_order_acquire); }

    /// Sets parameter `id` to `value` at the start of the next block. False
    /// if the queue is full (try again later). The built-ins take the value
    /// at once and never refuse (AudioProcessor::parameterChangedByHost()).
    bool setParameter(std::uint32_t id, double value);
    /// A bypassed instance passes audio through, unprocessed. As bypass
    /// starts, the processor is reset (held notes end, tails stop): the
    /// notes sent meanwhile are dropped.
    void setBypassed(bool bypassed) noexcept { bypassed_.store(bypassed, std::memory_order_relaxed); }
    [[nodiscard]] bool isBypassed() const noexcept { return bypassed_.load(std::memory_order_relaxed); }

    /// Frames the instance delays its sound by: the processor's latency as
    /// read at activate() or by the last updateLatency() (at most
    /// kMaxLatencyFrames); 0 while inactive. Bypassed, it delays the dry
    /// sound as much (up to the bypass line's room, see updateLatency()), so
    /// the mixer's delay compensation (ADR 0007) stays right. Control thread.
    [[nodiscard]] int latencyFrames() const noexcept
    {
        return active_.load() ? latency_.load(std::memory_order_relaxed) : 0;
    }

    /// Reads the processor's latency again; true if it changed. Plugins
    /// change it while running, e.g. when oversampling or a linear-phase mode
    /// is switched on in their editor. The bypass line was sized at
    /// activate() with room to spare (kBypassHeadroom); past that, the dry
    /// sound is delayed only as far as it fits until the next activate().
    /// Control thread; Engine::updatePluginLatency() calls it.
    bool updateLatency();

    /// Room the bypass line gets beyond the latency at activate(), in frames.
    static constexpr int kBypassHeadroom = 8192;

    /// The processor, for what only the control thread may ask (info,
    /// parameters, state).
    [[nodiscard]] AudioProcessor& processor() noexcept { return *processor_; }
    [[nodiscard]] const AudioProcessor& processor() const noexcept { return *processor_; }

    /// Notes one process() call passes on; more are dropped.
    static constexpr std::size_t kMaxNotes = 256;

    /// Audio thread: processes `numFrames` of stereo in place (blocks longer
    /// than the activation's maxBlockFrames are split), with the queued
    /// parameter changes at frame 0 (the latest per parameter) and `notes`
    /// (in frame order; frames past the block land on its last frame).
    /// Inactive or bypassed: the audio passes through and the notes are
    /// dropped; parameter changes made meanwhile are kept for the next block
    /// processed.
    void process(float* left, float* right, int numFrames, const TransportInfo& transport,
        std::span<const NoteEvent> notes = {}) noexcept;

    /// Stereo outputs after the main one that one process() call can fill.
    static constexpr std::size_t kMaxExtraOutputs = 16;
    /// What else one process() call may carry (all optional).
    struct Io {
        /// The sidechain: two channels of numFrames each, or both null.
        const float* sidechainLeft = nullptr;
        const float* sidechainRight = nullptr;
        /// Buffers of numFrames for the outputs after the main one, as in
        /// ProcessContext::extraOutputs; at most 2 * kMaxExtraOutputs.
        /// Written with silence while the instance is inactive or bypassed.
        std::span<float* const> extraOutputs{};
        /// Room for the notes the processor makes, and how many it made.
        std::span<NoteEvent> notesOut{};
        int* notesOutCount = nullptr;
    };
    /// As above, with a sidechain, extra outputs and notes out.
    void process(float* left, float* right, int numFrames, const TransportInfo& transport,
        std::span<const NoteEvent> notes, const Io& io) noexcept;
    /// Audio thread: forwards to AudioProcessor::reset() when active.
    void reset() noexcept;

    /// Notes played by hand that can wait for the audio thread; more are refused.
    static constexpr std::size_t kMaxLiveNotes = 63;
    /// Plays a note that is not in the song (an editor's keyboard, a MIDI
    /// keyboard): `key` 0 .. 127 starts at `velocity` (0 .. 1) with the next
    /// block processed, and sounds until stopNote(). False if too many wait.
    /// While inactive or bypassed they are dropped, like the song's notes.
    /// Control thread (one thread at a time).
    bool playNote(int key, float velocity);
    bool stopNote(int key);

    /// Controls that can wait for the audio thread; more are refused.
    static constexpr std::size_t kMaxLiveControls = 255;
    /// Sends a pitch bend, a controller or pressure (a MIDI keyboard's
    /// wheels and pedals) with the next block processed. False if too many
    /// wait or the value is not a number. Dropped while inactive or
    /// bypassed. Control thread (one thread at a time).
    bool sendControl(ControlEvent::Type type, int number, float value);

    /// Frames the scope remembers: the most readScope() can return.
    static constexpr std::size_t kScopeFrames = 8192;
    /// Switches the scope on or off: while on, the instance keeps a copy of
    /// the sound it puts out (left and right averaged; the dry sound while
    /// bypassed) for an editor to draw, e.g. as a spectrum. Off (the
    /// default) it costs nothing. Control thread; the first call that
    /// switches it on allocates its memory.
    void setScopeEnabled(bool enabled);
    [[nodiscard]] bool isScopeEnabled() const noexcept { return scopeOn_.load(std::memory_order_relaxed); }
    /// Copies the newest out.size() frames the instance put out into `out`,
    /// oldest first, and returns how many: fewer than asked when it has not
    /// put out that many since the scope was switched on, 0 while it is off.
    /// For drawing only: frames written while this reads may be from either
    /// side of the write. Any thread but the audio thread.
    std::size_t readScope(std::span<float> out) const noexcept;

private:
    std::unique_ptr<AudioProcessor> processor_;
    std::atomic<bool> active_{false};
    std::atomic<bool> bypassed_{false};
    int maxBlockFrames_ = 0; ///< written before activation is published
    std::atomic<int> latency_{0}; ///< control thread writes, audio thread reads (bypass)
    DelayLine bypassLine_; ///< the dry path while bypassed: room for latency_ plus kBypassHeadroom
    SpscQueue<ParameterEvent, kMaxQueuedParameters + 1> parameters_;
    /// Audio thread: changes not given to the processor yet, one per parameter.
    std::array<ParameterEvent, kMaxQueuedParameters> pending_{};
    std::size_t pendingCount_ = 0;
    bool bypassedLast_ = false; ///< audio thread: the last block was bypassed
    /// Audio thread: the notes of one chunk, their frames made chunk-relative.
    std::array<NoteEvent, kMaxNotes> chunkNotes_{};

    SpscQueue<NoteEvent, kMaxLiveNotes + 1> liveNotes_; ///< played by hand, for the next block
    SpscQueue<ControlEvent, kMaxLiveControls + 1> liveControls_;
    /// Audio thread: the controls of one block (they go with its first chunk).
    std::array<ControlEvent, kMaxLiveControls> blockControls_{};

    /// Audio thread: adds `numFrames` of output to the scope when it is on.
    void capture(const float* left, const float* right, int numFrames) noexcept;
    /// The scope's ring of kScopeFrames samples; allocated when first switched
    /// on and kept. Atomic samples: the reader may race the writer.
    std::unique_ptr<std::atomic<float>[]> scope_;
    std::atomic<bool> scopeOn_{false};
    std::atomic<std::uint64_t> scopeWritten_{0}; ///< frames written since it was switched on
};

} // namespace vloom
