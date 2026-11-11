// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/dsp/Metronome.hpp"
#include "vloom/engine/time/Transport.hpp"

#include <array>
#include <atomic>

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
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void prepare(double sampleRate, int maxBlockSize);

    /// Renders numFrames of non-interleaved audio into `out`.
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

    // ---- status (any thread)
    [[nodiscard]] bool isPlaying() const noexcept { return playing_.load(std::memory_order_relaxed); }
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
    void measureLoad(double seconds, int numFrames) noexcept;

    SpscQueue<Command, 256> commands_;
    Transport transport_;
    Metronome metronome_;
    bool metronomeEnabled_ = true;
    float masterGainTarget_ = 1.0f; ///< audio thread: latest requested gain
    float masterPanTarget_ = 0.0f; ///< audio thread: latest requested balance
    /// audio thread: per-channel gain at the end of the last block (ramp start)
    std::array<float, 8> channelGain_{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float smoothedLoad_ = 0.0f; ///< audio thread

    std::atomic<bool> playing_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> tempo_{120.0};
    std::atomic<float> cpuLoad_{0.0f};
    std::array<std::atomic<float>, 2> outputPeak_{}; ///< left, right; see takeOutputPeak()
};

} // namespace vloom
