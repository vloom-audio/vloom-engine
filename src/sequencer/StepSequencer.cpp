// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sequencer/StepSequencer.hpp"

namespace vloom {

void StepSequencer::beginBlock() noexcept { current_ = exchange_.acquire(); }

void StepSequencer::endBlock() noexcept
{
    exchange_.release([this](const SequencerState& old) { return canRelease(old); });
}

int StepSequencer::ticksPerStep() const noexcept
{
    return current_ != nullptr && current_->stepCount > 0 ? current_->ticksPerStep : 0;
}

void StepSequencer::triggerStep(std::int64_t step) noexcept
{
    if (current_ == nullptr || current_->stepCount <= 0 || step < 0)
        return;
    const auto index = static_cast<std::size_t>(step % current_->stepCount);
    for (const ChannelState& channel : current_->channels) {
        if (channel.muted || channel.voices == nullptr || index >= channel.steps.size()
            || channel.steps[index] == 0)
            continue;
        channel.voices->sampler.trigger(channel.sample.get(), channel.gain, channel.pan);
    }
}

void StepSequencer::render(float* const* out, int numChannels, int startFrame, int numFrames) noexcept
{
    ++pass_;
    const auto play = [&](ChannelVoices* voices) {
        if (voices == nullptr || voices->renderedPass == pass_)
            return;
        voices->renderedPass = pass_;
        voices->sampler.render(out, numChannels, startFrame, numFrames);
    };
    if (current_ != nullptr)
        for (const ChannelState& channel : current_->channels)
            play(channel.voices.get());
    // Removed channels: their last notes fade out (stopAll() only starts a
    // fade once per note) and are rendered from the old state until silent.
    exchange_.forEachDraining([&](const SequencerState& old) {
        for (const ChannelState& channel : old.channels) {
            ChannelVoices* voices = channel.voices.get();
            if (voices == nullptr || voices->sampler.activeVoices() == 0 || inCurrent(voices))
                continue;
            voices->sampler.stopAll();
            play(voices);
        }
    });
}

void StepSequencer::stopAll() noexcept
{
    const auto stop = [](const ChannelState& channel) {
        if (channel.voices != nullptr)
            channel.voices->sampler.stopAll();
    };
    if (current_ != nullptr)
        for (const ChannelState& channel : current_->channels)
            stop(channel);
    exchange_.forEachDraining([&](const SequencerState& old) {
        for (const ChannelState& channel : old.channels)
            stop(channel);
    });
}

bool StepSequencer::inCurrent(const ChannelVoices* voices) const noexcept
{
    if (current_ == nullptr)
        return false;
    for (const ChannelState& channel : current_->channels)
        if (channel.voices.get() == voices)
            return true;
    return false;
}

bool StepSequencer::canRelease(const SequencerState& old) const noexcept
{
    // ADR 0005: an old state may go once nothing it alone owns is still
    // needed. Its voice objects must be silent unless the current state still
    // has them. Its samples must not be played by ANY sampler still rendered,
    // unless the current state holds them too: two channels can share one
    // sample, and the one still playing it may belong to another channel.
    for (const ChannelState& channel : old.channels) {
        const ChannelVoices* voices = channel.voices.get();
        if (voices != nullptr && !inCurrent(voices) && voices->sampler.activeVoices() > 0)
            return false; // a removed channel still fading
        const SampleBuffer* sample = channel.sample.get();
        if (sample != nullptr && !sampleInCurrent(sample) && samplePlaying(sample))
            return false; // a sample only this state owns is still sounding
    }
    return true;
}

bool StepSequencer::sampleInCurrent(const SampleBuffer* sample) const noexcept
{
    if (current_ == nullptr)
        return false;
    for (const ChannelState& channel : current_->channels)
        if (channel.sample.get() == sample)
            return true;
    return false;
}

bool StepSequencer::samplePlaying(const SampleBuffer* sample) const noexcept
{
    const auto plays = [sample](const SequencerState& state) {
        for (const ChannelState& channel : state.channels)
            if (channel.voices != nullptr && channel.voices->sampler.uses(sample))
                return true;
        return false;
    };
    if (current_ != nullptr && plays(*current_))
        return true;
    bool playing = false;
    exchange_.forEachDraining([&](const SequencerState& state) { playing = playing || plays(state); });
    return playing;
}

} // namespace vloom
