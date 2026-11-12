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
        // Volume and pan are applied to the whole channel in mix(), live.
        channel.voices->sampler.trigger(channel.sample.get());
    }
}

void StepSequencer::render(int startFrame, int numFrames) noexcept
{
    ++pass_;
    const auto play = [&](ChannelVoices* voices) {
        if (voices == nullptr || voices->renderedPass == pass_ || voices->sampler.activeVoices() == 0)
            return;
        voices->renderedPass = pass_;
        float* const buffer[2] = {voices->left.data(), voices->right.data()};
        voices->sampler.render(buffer, 2, startFrame, numFrames);
        voices->written = true;
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

void StepSequencer::mix(int numFrames, float* const* buses, bool* used) noexcept
{
    ++block_;
    const auto mixChannel = [&](const ChannelState& channel) {
        ChannelVoices* voices = channel.voices.get();
        if (voices == nullptr || voices->mixedBlock == block_)
            return;
        voices->mixedBlock = block_;
        const StereoGain target = balance(channel.gain, channel.pan);
        if (!voices->rampStarted) { // first block: no ramp from a stale value
            voices->rampStarted = true;
            voices->applied = target;
        }
        if (!voices->written) { // silent: the next sound starts at the current setting
            voices->applied = target;
            return;
        }
        const int insert = channel.insert >= 0 && channel.insert <= kMaxInserts ? channel.insert : 0;
        float* outLeft = buses[2 * insert];
        float* outRight = buses[2 * insert + 1];
        used[insert] = true;

        // Linear ramp across the block, reaching the target on its last frame.
        const float n = static_cast<float>(numFrames);
        const float stepLeft = (target.left - voices->applied.left) / n;
        const float stepRight = (target.right - voices->applied.right) / n;
        float* left = voices->left.data();
        float* right = voices->right.data();
        for (int i = 0; i < numFrames; ++i) {
            const float k = static_cast<float>(i + 1);
            outLeft[i] += left[i] * (voices->applied.left + stepLeft * k);
            outRight[i] += right[i] * (voices->applied.right + stepRight * k);
            left[i] = 0.0f;
            right[i] = 0.0f;
        }
        voices->applied = target;
        voices->written = false;
    };
    if (current_ != nullptr)
        for (const ChannelState& channel : current_->channels)
            mixChannel(channel);
    // Removed channels still fading: through the insert of their last state.
    exchange_.forEachDraining([&](const SequencerState& old) {
        for (const ChannelState& channel : old.channels)
            if (channel.voices != nullptr && !inCurrent(channel.voices.get()))
                mixChannel(channel);
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
