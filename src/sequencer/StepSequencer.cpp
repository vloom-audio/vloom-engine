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
    // playing. Its samples may be in use if the current state shares them
    // (they stay alive there); its voice objects must be silent unless the
    // current state still has them.
    for (const ChannelState& channel : old.channels) {
        const ChannelVoices* voices = channel.voices.get();
        if (voices == nullptr)
            continue;
        const bool voicesKept = inCurrent(voices);
        if (!voicesKept && voices->sampler.activeVoices() > 0)
            return false; // a removed channel still fading
        if (channel.sample == nullptr || !voices->sampler.uses(channel.sample.get()))
            continue;
        bool sampleKept = false;
        if (current_ != nullptr)
            for (const ChannelState& now : current_->channels)
                if (now.sample == channel.sample) {
                    sampleKept = true;
                    break;
                }
        if (!sampleKept)
            return false; // a replaced sample still sounding
    }
    return true;
}

} // namespace vloom
