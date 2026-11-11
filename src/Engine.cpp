// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/Engine.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vloom {

void Engine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    // Called while audio is stopped, so touching audio-thread state is safe.
    transport_.prepare(sampleRate);
    metronome_.prepare(sampleRate);

    Command discarded{};
    while (commands_.pop(discarded)) { }
    applyCommands();
    publishStatus();
}

bool Engine::send(Command::Type type, double value) { return commands_.push(Command{type, value}); }

bool Engine::play() { return send(Command::Type::Play); }
bool Engine::stop() { return send(Command::Type::Stop); }
bool Engine::locate(double ticks) { return send(Command::Type::Locate, ticks); }
bool Engine::setTempo(double bpm) { return send(Command::Type::SetTempo, bpm); }
bool Engine::setMetronomeEnabled(bool enabled)
{
    return send(Command::Type::SetMetronome, enabled ? 1.0 : 0.0);
}

void Engine::applyCommands() noexcept
{
    Command cmd{};
    while (commands_.pop(cmd)) {
        switch (cmd.type) {
        case Command::Type::Play:
            transport_.play();
            break;
        case Command::Type::Stop:
            transport_.stop();
            break;
        case Command::Type::Locate:
            transport_.locate(cmd.value);
            break;
        case Command::Type::SetTempo:
            transport_.setTempo(cmd.value);
            break;
        case Command::Type::SetMetronome:
            metronomeEnabled_ = cmd.value != 0.0;
            break;
        }
    }
}

void Engine::publishStatus() noexcept
{
    playing_.store(transport_.isPlaying(), std::memory_order_relaxed);
    position_.store(transport_.positionTicks(), std::memory_order_relaxed);
    tempo_.store(transport_.tempo(), std::memory_order_relaxed);
}

void Engine::process(float* const* out, int numChannels, int numFrames) noexcept
{
    applyCommands();

    for (int ch = 0; ch < numChannels; ++ch)
        std::memset(out[ch], 0, sizeof(float) * static_cast<std::size_t>(numFrames));

    int cursor = 0;

    if (transport_.isPlaying()) {
        // Find every beat boundary inside this block and trigger the click on
        // the exact sample it falls on (sample-accurate scheduling).
        // A beat whose exact position is within kEpsilon frames of a sample
        // boundary snaps to that sample, so rounding noise never shifts it.
        constexpr double kEpsilon = 1e-6;
        const double start = transport_.positionTicks();

        auto beat = static_cast<std::int64_t>(std::floor(start / kPpq));
        for (;; ++beat) {
            const double beatTick = static_cast<double>(beat) * kPpq;
            const double frames = transport_.framesUntil(beatTick);
            if (frames < -kEpsilon)
                continue; // already played
            const int offset = static_cast<int>(std::ceil(frames - kEpsilon));
            if (offset >= numFrames)
                break;

            metronome_.render(out, numChannels, cursor, offset - cursor);
            cursor = offset;
            if (metronomeEnabled_)
                metronome_.trigger(beat % transport_.beatsPerBar() == 0);
        }
    }

    metronome_.render(out, numChannels, cursor, numFrames - cursor);
    transport_.advance(numFrames);
    publishStatus();
}

} // namespace vloom
