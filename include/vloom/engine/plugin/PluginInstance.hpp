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
    /// if the queue is full (try again later).
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
    /// Audio thread: forwards to AudioProcessor::reset() when active.
    void reset() noexcept;

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
};

} // namespace vloom
