// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/Engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace vloom {

Engine::Engine()
    : busMemory_(static_cast<std::size_t>(2 * (kMaxInserts + 1) * kMaxBlockFrames), 0.0f)
{
    for (std::size_t i = 0; i < buses_.size(); ++i)
        buses_[i] = busMemory_.data() + i * static_cast<std::size_t>(kMaxBlockFrames);
}

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
bool Engine::locate(double ticks)
{
    if (!std::isfinite(ticks))
        return true; // ignored, like setMasterGain()
    return send(Command::Type::Locate, std::clamp(ticks, 0.0, Transport::kMaxTicks));
}
bool Engine::setTempo(double bpm)
{
    if (!std::isfinite(bpm))
        return true; // ignored: NaN would stall the scheduler
    return send(Command::Type::SetTempo, bpm);
}
bool Engine::setMetronomeEnabled(bool enabled)
{
    return send(Command::Type::SetMetronome, enabled ? 1.0 : 0.0);
}
bool Engine::setMasterGain(float gain)
{
    if (std::isnan(gain))
        return true; // ignored, not an error worth retrying
    return send(Command::Type::SetMasterGain, static_cast<double>(std::clamp(gain, 0.0f, kMaxMasterGain)));
}
void Engine::setSequencerState(std::unique_ptr<const SequencerState> state)
{
    sequencer_.publish(std::move(state));
}

void Engine::collectGarbage()
{
    sequencer_.collect();
    mixer_.collect();
}

void Engine::setMixerState(std::unique_ptr<const MixerState> state) { mixer_.publish(std::move(state)); }

float Engine::takeInsertPeak(int insert, int channel) noexcept
{
    if (insert < 1 || insert > kMaxInserts || channel < 0 || channel > 1)
        return 0.0f;
    return insertPeak_[static_cast<std::size_t>(insert)][static_cast<std::size_t>(channel)].exchange(
        0.0f, std::memory_order_relaxed);
}

bool Engine::setMasterPan(float pan)
{
    if (std::isnan(pan))
        return true; // ignored, like setMasterGain()
    return send(Command::Type::SetMasterPan, static_cast<double>(std::clamp(pan, -1.0f, 1.0f)));
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
            stopVoices_ = true;
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
        case Command::Type::SetMasterGain:
            masterGainTarget_ = static_cast<float>(cmd.value);
            break;
        case Command::Type::SetMasterPan:
            masterPanTarget_ = static_cast<float>(cmd.value);
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

float Engine::channelGainTarget(int channel, int numChannels) const noexcept
{
    // Balance on the first two channels of a stereo (or wider) output: the
    // side panned away from is turned down, the other stays at full gain.
    if (numChannels < 2 || channel > 1)
        return masterGainTarget_;
    const float side = channel == 0 ? 1.0f - masterPanTarget_ : 1.0f + masterPanTarget_;
    return masterGainTarget_ * std::min(1.0f, side);
}

void Engine::applyMasterGain(float* const* out, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0)
        return;
    for (int ch = 0; ch < numChannels; ++ch) {
        const float end = channelGainTarget(ch, numChannels);
        // Channels beyond the ramp state jump to the target (no such device yet).
        const bool tracked = ch < static_cast<int>(channelGain_.size());
        const float start = tracked ? channelGain_[static_cast<std::size_t>(ch)] : end;
        float* samples = out[ch];
        if (start == end) {
            if (end != 1.0f)
                for (int i = 0; i < numFrames; ++i)
                    samples[i] *= end;
            continue;
        }
        // Linear ramp across the block, reaching the target on its last frame.
        const float step = (end - start) / static_cast<float>(numFrames);
        for (int i = 0; i < numFrames; ++i)
            samples[i] *= start + step * static_cast<float>(i + 1);
        channelGain_[static_cast<std::size_t>(ch)] = end;
    }
}

void Engine::sanitizeOutput(float* const* out, int numChannels, int numFrames) noexcept
{
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numFrames; ++i)
            if (!std::isfinite(out[ch][i]))
                out[ch][i] = 0.0f;
}

void Engine::meterOutput(float* const* out, int numChannels, int numFrames) noexcept
{
    const int metered = std::min(numChannels, static_cast<int>(outputPeak_.size()));
    for (int ch = 0; ch < metered; ++ch) {
        float peak = 0.0f;
        for (int i = 0; i < numFrames; ++i)
            peak = std::max(peak, std::abs(out[ch][i]));
        // Keep the highest peak until the UI takes it (lock-free max).
        auto& slot = outputPeak_[static_cast<std::size_t>(ch)];
        float seen = slot.load(std::memory_order_relaxed);
        while (peak > seen && !slot.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) { }
    }
}

float Engine::takeOutputPeak(int channel) noexcept
{
    if (channel < 0 || channel >= static_cast<int>(outputPeak_.size()))
        return 0.0f;
    return outputPeak_[static_cast<std::size_t>(channel)].exchange(0.0f, std::memory_order_relaxed);
}

void Engine::measureLoad(double seconds, int numFrames) noexcept
{
    const double sampleRate = transport_.sampleRate();
    if (numFrames <= 0 || sampleRate <= 0.0)
        return;
    const double budget = static_cast<double>(numFrames) / sampleRate;
    const auto load = static_cast<float>(seconds / budget);
    // One-pole smoothing so the meter reads steadily; rises fast, falls slow.
    constexpr float kRise = 0.5f;
    constexpr float kFall = 0.05f;
    smoothedLoad_ += (load - smoothedLoad_) * (load > smoothedLoad_ ? kRise : kFall);
    cpuLoad_.store(smoothedLoad_, std::memory_order_relaxed);
}

void Engine::renderSegment(float* const* out, int numChannels, int startFrame, int numFrames) noexcept
{
    if (numFrames <= 0)
        return;
    metronome_.render(out, numChannels, startFrame, numFrames);
    sequencer_.render(startFrame, numFrames);
}

void Engine::process(float* const* out, int numChannels, int numFrames) noexcept
{
    // steady_clock::now() does not allocate or lock.
    const auto started = std::chrono::steady_clock::now();

    // Buses hold kMaxBlockFrames: longer requests are rendered in parts.
    constexpr int kMaxChannels = 8;
    for (int ch = kMaxChannels; ch < numChannels; ++ch) // beyond what is rendered: silence
        std::memset(out[ch], 0, sizeof(float) * static_cast<std::size_t>(std::max(numFrames, 0)));
    const int channels = std::min(numChannels, kMaxChannels);
    std::array<float*, kMaxChannels> part{};
    for (int done = 0; done < numFrames;) {
        const int frames = std::min(numFrames - done, kMaxBlockFrames);
        for (int ch = 0; ch < channels; ++ch)
            part[static_cast<std::size_t>(ch)] = out[ch] + done;
        processBlock(part.data(), channels, frames);
        done += frames;
    }

    measureLoad(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), numFrames);
}

void Engine::mixBuses(float* const* out, int numChannels, int numFrames) noexcept
{
    const MixerState* state = mixer_.acquire();
    float* masterLeft = buses_[0];
    float* masterRight = buses_[1];
    const float n = static_cast<float>(numFrames);
    for (int insert = 1; insert <= kMaxInserts; ++insert) {
        const auto index = static_cast<std::size_t>(insert);
        const InsertState settings
            = state != nullptr && index <= state->inserts.size() ? state->inserts[index - 1] : InsertState{};
        const StereoGain target = balance(settings.gain, settings.pan);
        StereoGain& applied = insertApplied_[index];
        if (!busUsed_[index]) {
            applied = target; // silent: nothing to ramp
            insertSounding_[index] = false;
            continue;
        }
        if (!insertSounding_[index])
            applied = target; // was silent: start at the setting, no ramp from a stale one
        insertSounding_[index] = true;
        busUsed_[index] = false;
        float* left = buses_[2 * index];
        float* right = buses_[2 * index + 1];
        const float stepLeft = (target.left - applied.left) / n;
        const float stepRight = (target.right - applied.right) / n;
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        for (int i = 0; i < numFrames; ++i) {
            const float k = static_cast<float>(i + 1);
            const float l = left[i] * (applied.left + stepLeft * k);
            const float r = right[i] * (applied.right + stepRight * k);
            masterLeft[i] += l;
            masterRight[i] += r;
            peakLeft = std::max(peakLeft, std::abs(l));
            peakRight = std::max(peakRight, std::abs(r));
            left[i] = 0.0f;
            right[i] = 0.0f;
        }
        applied = target;
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const float peak = ch == 0 ? peakLeft : peakRight;
            auto& slot = insertPeak_[index][ch];
            float seen = slot.load(std::memory_order_relaxed);
            while (peak > seen && !slot.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) { }
        }
    }
    mixer_.release([](const MixerState&) { return true; }); // plain values: nothing to wait for

    // The master bus (direct channels plus inserts) into the output.
    for (int i = 0; i < numFrames; ++i) {
        if (numChannels >= 2) {
            out[0][i] += masterLeft[i];
            out[1][i] += masterRight[i];
        } else if (numChannels == 1) {
            out[0][i] += 0.5f * (masterLeft[i] + masterRight[i]);
        }
        masterLeft[i] = 0.0f;
        masterRight[i] = 0.0f;
    }
    busUsed_[0] = false;
}

void Engine::processBlock(float* const* out, int numChannels, int numFrames) noexcept
{
    stopVoices_ = false;
    applyCommands();
    sequencer_.beginBlock();
    if (stopVoices_)
        sequencer_.stopAll();

    for (int ch = 0; ch < numChannels; ++ch)
        std::memset(out[ch], 0, sizeof(float) * static_cast<std::size_t>(numFrames));

    int cursor = 0;

    if (transport_.isPlaying() && std::isfinite(transport_.positionTicks())) {
        // Two event streams, metronome beats and sequencer steps, merged in
        // time order. Each event fires on the first sample at or after its
        // exact time: the block is rendered in segments split at the events.
        // An event within kEpsilon frames of a sample boundary snaps to that
        // sample, so rounding noise never shifts it. One rule decides both
        // "later block" and "already played", so an event between the last
        // sample of one block and the first of the next fires exactly once.
        constexpr double kEpsilon = 1e-6;
        const double start = transport_.positionTicks();
        const int ticksPerStep = sequencer_.ticksPerStep();
        const double stepTicks = static_cast<double>(ticksPerStep);

        auto beat = static_cast<std::int64_t>(std::floor(start / kPpq));
        auto step = ticksPerStep > 0 ? static_cast<std::int64_t>(std::floor(start / stepTicks)) : 0;
        for (;;) {
            const double beatTick = static_cast<double>(beat) * kPpq;
            const double stepTick = ticksPerStep > 0 ? static_cast<double>(step) * stepTicks
                                                     : std::numeric_limits<double>::infinity();
            const double tick = std::min(beatTick, stepTick);
            const double at = std::ceil(transport_.framesUntil(tick) - kEpsilon); // sample it fires on
            if (at < 0.0) { // fired in an earlier block
                if (beatTick == tick)
                    ++beat;
                if (stepTick == tick)
                    ++step;
                continue;
            }
            if (at >= static_cast<double>(numFrames))
                break; // a later block
            const int offset = static_cast<int>(at);

            renderSegment(out, numChannels, cursor, offset - cursor);
            cursor = offset;
            if (beatTick == tick) {
                if (metronomeEnabled_)
                    metronome_.trigger(beat % transport_.beatsPerBar() == 0);
                ++beat;
            }
            if (stepTick == tick) {
                sequencer_.triggerStep(step);
                ++step;
            }
        }
    }

    renderSegment(out, numChannels, cursor, numFrames - cursor);
    sequencer_.mix(numFrames, buses_.data(), busUsed_.data());
    mixBuses(out, numChannels, numFrames);
    sequencer_.endBlock();
    applyMasterGain(out, numChannels, numFrames);
    sanitizeOutput(out, numChannels, numFrames);
    meterOutput(out, numChannels, numFrames);
    transport_.advance(numFrames);
    publishStatus();
}

} // namespace vloom
