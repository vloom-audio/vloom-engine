// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/dsp/Metronome.hpp"
#include "vloom/engine/time/Transport.hpp"

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

    // ---- status (any thread)
    [[nodiscard]] bool isPlaying() const noexcept { return playing_.load(std::memory_order_relaxed); }
    [[nodiscard]] double positionTicks() const noexcept { return position_.load(std::memory_order_relaxed); }
    [[nodiscard]] double tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }
    [[nodiscard]] double sampleRate() const noexcept { return transport_.sampleRate(); }
    [[nodiscard]] int beatsPerBar() const noexcept { return transport_.beatsPerBar(); }

private:
    struct Command {
        enum class Type { Play, Stop, Locate, SetTempo, SetMetronome };
        Type type;
        double value;
    };

    bool send(Command::Type type, double value = 0.0);
    void applyCommands() noexcept;
    void publishStatus() noexcept;

    SpscQueue<Command, 256> commands_;
    Transport transport_;
    Metronome metronome_;
    bool metronomeEnabled_ = true;

    std::atomic<bool> playing_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> tempo_{120.0};
};

} // namespace vloom
