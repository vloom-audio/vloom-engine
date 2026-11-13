// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/dsp/Metronome.hpp"
#include "vloom/engine/dsp/Sampler.hpp"
#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/sequencer/StepSequencer.hpp"
#include "vloom/engine/time/Transport.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace vloom {

/// The audio engine.
///
/// Threading contract
///  - prepare() is called from a non-real-time thread while audio is stopped.
///  - process() is called only from the audio thread. It never allocates,
///    locks, or does I/O.
///  - Control calls (play, stop, setTempo, ...) may be made from ONE control
///    thread (usually the UI thread). They are queued and applied at the start
///    of the next audio block.
///  - Status getters may be called from any thread.
class Engine {
public:
    /// Allocates the mixer's buses (about 4 MB), so process() never has to.
    Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    /// Renders numFrames of non-interleaved audio into `out`. Blocks longer
    /// than kMaxBlockFrames are rendered in parts.
    void process(float* const* out, int numChannels, int numFrames) noexcept;

    // ---- control (one control thread) — return false if the queue is full
    bool play();
    bool stop();
    bool locate(double ticks);
    bool setTempo(double bpm);
    bool setMetronomeEnabled(bool enabled);
    /// Linear output gain, clamped to [0, kMaxMasterGain]; NaN is ignored.
    /// Changes ramp over one block, so moving a knob does not click.
    bool setMasterGain(float gain);
    /// Stereo balance of the output, -1 (left) .. 0 (centre) .. +1 (right),
    /// clamped; NaN is ignored. Ramps like the gain.
    bool setMasterPan(float pan);

    static constexpr float kMaxMasterGain = 2.0f;

    /// Replaces the channel rack (channels, samples, steps) the audio thread
    /// plays, from the next block on. Ringing notes are not cut: keep each
    /// channel's ChannelVoices across states (docs/adr/0005).
    void setSequencerState(std::unique_ptr<const SequencerState> state);
    /// Frees states the audio thread has finished with. Call it regularly
    /// from the control thread (a UI timer); setSequencerState() does too.
    void collectGarbage();
    /// Replaces the mixer inserts' volume and pan (mute and solo folded into
    /// the gain by the host), from the next block on; changes ramp.
    void setMixerState(std::unique_ptr<const MixerState> state);
    /// Plays `sample` once from its start on the master bus (after the master
    /// volume), whether or not the transport plays: a browser preview. A new
    /// preview replaces the one sounding, which fades out; null stops it.
    /// The sample must be at the engine's rate (sample/Resampler). Stop
    /// (stop()) silences it too.
    void previewSample(std::shared_ptr<const SampleBuffer> sample, float gain = 1.0f);

    // ---- status (any thread)
    [[nodiscard]] bool isPlaying() const noexcept { return playing_.load(std::memory_order_relaxed); }
    /// True while a preview (previewSample()) is sounding, its fade included.
    [[nodiscard]] bool isPreviewPlaying() const noexcept
    {
        return previewPlaying_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double positionTicks() const noexcept { return position_.load(std::memory_order_relaxed); }
    [[nodiscard]] double tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }
    [[nodiscard]] double sampleRate() const noexcept { return transport_.sampleRate(); }
    [[nodiscard]] int beatsPerBar() const noexcept { return transport_.beatsPerBar(); }
    /// Smoothed share of each block's real-time budget that process() used:
    /// 0 = idle, 1 = the whole block (audio starts to drop out above that).
    [[nodiscard]] float cpuLoad() const noexcept { return cpuLoad_.load(std::memory_order_relaxed); }
    /// Highest absolute sample on output `channel` (0 = left, 1 = right) since
    /// the last call, after master gain and pan; resets it to 0. For one
    /// meter reader (the UI); other channels read 0.
    [[nodiscard]] float takeOutputPeak(int channel) noexcept;
    /// Like takeOutputPeak() for mixer insert `insert` (1 .. kMaxInserts),
    /// after its volume and pan.
    [[nodiscard]] float takeInsertPeak(int insert, int channel) noexcept;

private:
    struct Command {
        enum class Type { Play, Stop, Locate, SetTempo, SetMetronome, SetMasterGain, SetMasterPan };
        Type type;
        double value;
    };

    bool send(Command::Type type, double value = 0.0);
    void applyCommands() noexcept;
    void publishStatus() noexcept;
    void applyMasterGain(float* const* out, int numChannels, int numFrames) noexcept;
    [[nodiscard]] float channelGainTarget(int channel, int numChannels) const noexcept;
    void meterOutput(float* const* out, int numChannels, int numFrames) noexcept;
    /// NaN / infinity become silence before the device sees them.
    static void sanitizeOutput(float* const* out, int numChannels, int numFrames) noexcept;
    void measureLoad(double seconds, int numFrames) noexcept;
    void renderSegment(float* const* out, int numChannels, int startFrame, int numFrames) noexcept;
    void processBlock(float* const* out, int numChannels, int numFrames) noexcept;
    /// Inserts (volume, pan, meters) into the master bus, the master bus into `out`.
    void mixBuses(float* const* out, int numChannels, int numFrames) noexcept;
    /// Starts a newly published preview and adds the preview into the master bus.
    void renderPreview(int numFrames) noexcept;

    /// What the preview voice plays; one per previewSample() call.
    struct PreviewState {
        std::shared_ptr<const SampleBuffer> sample; ///< null: stop
        float gain = 1.0f;
    };

    SpscQueue<Command, 256> commands_;
    Transport transport_;
    Metronome metronome_;
    StepSequencer sequencer_;
    SnapshotExchange<MixerState> mixer_;
    SnapshotExchange<PreviewState> preview_;
    Sampler previewSampler_{1}; ///< audio thread: one voice (plus the sampler's fade slots)
    const PreviewState* previewStarted_ = nullptr; ///< audio thread: the preview last triggered
    /// audio thread: stereo bus per insert, 0 = master bus; kMaxBlockFrames each
    std::vector<float> busMemory_;
    std::array<float*, 2 * (kMaxInserts + 1)> buses_{};
    std::array<bool, kMaxInserts + 1> busUsed_{};
    std::array<StereoGain, kMaxInserts + 1> insertApplied_{}; ///< audio thread: ramp start per insert
    std::array<bool, kMaxInserts + 1> insertSounding_{}; ///< audio thread: the insert had sound last block
    std::array<std::array<std::atomic<float>, 2>, kMaxInserts + 1> insertPeak_{};
    bool stopVoices_ = false; ///< audio thread: a Stop command arrived this block
    bool wasRolling_ = false; ///< audio thread: the transport played in the last block
    double expectedTicks_ = 0.0; ///< audio thread: where the last block left the position
    bool metronomeEnabled_ = true;
    float masterGainTarget_ = 1.0f; ///< audio thread: latest requested gain
    float masterPanTarget_ = 0.0f; ///< audio thread: latest requested balance
    /// audio thread: per-channel gain at the end of the last block (ramp start)
    std::array<float, 8> channelGain_{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float smoothedLoad_ = 0.0f; ///< audio thread

    std::atomic<bool> playing_{false};
    std::atomic<bool> previewPlaying_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> tempo_{120.0};
    std::atomic<float> cpuLoad_{0.0f};
    std::array<std::atomic<float>, 2> outputPeak_{}; ///< left, right; see takeOutputPeak()
};

} // namespace vloom
