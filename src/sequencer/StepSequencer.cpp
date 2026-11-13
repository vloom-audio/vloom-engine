// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sequencer/StepSequencer.hpp"

#include <algorithm>
#include <cmath>

namespace vloom {

namespace {

/// A note that can play in a pattern (or clip) `span` ticks long.
bool playable(const Note& note, std::int64_t span) noexcept
{
    return note.start >= 0 && note.start < span && note.length > 0 && note.key >= Note::kMinKey
        && note.key <= Note::kMaxKey && std::isfinite(note.velocity);
}

/// Equal temperament, A4 (MIDI 69) = 440 Hz.
double keyFrequency(int key) noexcept { return 440.0 * std::exp2(static_cast<double>(key - 69) / 12.0); }

/// An audio clip plays at the sample's own speed, master pitch or not, so it
/// stays in time with the song.
Sampler::PlayOptions audioClipOptions(std::int64_t startFrame) noexcept
{
    Sampler::PlayOptions options;
    options.startFrame = startFrame;
    options.followsPitch = false;
    return options;
}

} // namespace

template <typename Visit> void StepSequencer::forEachNote(std::int64_t from, Visit&& visit) const noexcept
{
    if (current_ == nullptr || current_->stepCount <= 0 || current_->ticksPerStep <= 0 || from < 0)
        return;
    const std::int64_t ticksPerStep = current_->ticksPerStep;
    const auto& channels = current_->channels;
    if (current_->song) {
        for (const SongPatternClip& clip : current_->patternClips) {
            if (clip.pattern < 0 || static_cast<std::size_t>(clip.pattern) >= current_->patterns.size()
                || clip.startStep < 0 || clip.lengthSteps <= 0)
                continue;
            const std::int64_t start = clip.startStep * ticksPerStep;
            const std::int64_t span = clip.lengthSteps * ticksPerStep;
            if (start + span <= from)
                continue; // over already
            const auto& rows = current_->patterns[static_cast<std::size_t>(clip.pattern)].notes;
            for (std::size_t c = 0; c < channels.size() && c < rows.size(); ++c)
                for (const Note& note : rows[c])
                    if (playable(note, span) && start + note.start >= from)
                        visit(channels[c], note, start + note.start);
        }
        return;
    }
    // Pattern mode: the pattern loops, so each note comes round once per period.
    const std::int64_t period = static_cast<std::int64_t>(current_->stepCount) * ticksPerStep;
    const std::int64_t loop = from / period;
    const std::int64_t local = from - loop * period;
    for (const ChannelState& channel : channels)
        for (const Note& note : channel.notes)
            if (playable(note, period))
                visit(channel, note, (note.start >= local ? loop : loop + 1) * period + note.start);
}

std::int64_t StepSequencer::nextNoteStart(std::int64_t from) const noexcept
{
    std::int64_t next = kNoEvent;
    forEachNote(
        from, [&next](const ChannelState&, const Note&, std::int64_t at) { next = std::min(next, at); });
    return next;
}

void StepSequencer::startNotes(std::int64_t tick) noexcept
{
    forEachNote(tick, [this, tick](const ChannelState& channel, const Note& note, std::int64_t at) {
        if (at == tick)
            playNote(channel, note, tick + note.length);
    });
}

void StepSequencer::playNote(const ChannelState& channel, const Note& note, std::int64_t end) const noexcept
{
    if (channel.muted || channel.voices == nullptr)
        return;
    if (channel.instrument == Instrument::Synth) {
        channel.voices->synth.noteOn(keyFrequency(note.key), note.velocity, end);
        return;
    }
    if (channel.sample == nullptr)
        return;
    const int root = std::clamp(channel.rootKey, Note::kMinKey, Note::kMaxKey);
    Sampler::PlayOptions options;
    options.gain = std::clamp(note.velocity, 0.0f, 1.0f);
    options.rate = std::exp2(static_cast<double>(note.key - root) / 12.0);
    options.releaseAt = end;
    channel.voices->sampler.play(channel.sample.get(), options);
}

void StepSequencer::playStep(const ChannelState& channel, std::int64_t step) const noexcept
{
    // Volume and pan are applied to the whole channel in mix(), live.
    if (channel.instrument == Instrument::Synth) {
        const int root = std::clamp(channel.rootKey, Note::kMinKey, Note::kMaxKey);
        channel.voices->synth.noteOn(keyFrequency(root), 1.0f, (step + 1) * current_->ticksPerStep);
        return;
    }
    channel.voices->sampler.trigger(channel.sample.get());
}

std::int64_t StepSequencer::nextNoteEnd() const noexcept
{
    std::int64_t next = kNoEvent;
    if (current_ != nullptr)
        for (const ChannelState& channel : current_->channels)
            if (channel.voices != nullptr)
                next = std::min(next, channel.voices->nextRelease());
    return next;
}

void StepSequencer::endNotes(std::int64_t upTo) noexcept
{
    if (current_ != nullptr)
        for (const ChannelState& channel : current_->channels)
            if (channel.voices != nullptr)
                channel.voices->release(upTo);
}

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
    if (current_->song) {
        const auto& channels = current_->channels;
        for (const SongPatternClip& clip : current_->patternClips) {
            if (step < clip.startStep || step >= clip.startStep + clip.lengthSteps || clip.pattern < 0
                || static_cast<std::size_t>(clip.pattern) >= current_->patterns.size())
                continue;
            const auto& rows = current_->patterns[static_cast<std::size_t>(clip.pattern)].steps;
            const auto local = static_cast<std::size_t>(step - clip.startStep);
            for (std::size_t c = 0; c < channels.size() && c < rows.size(); ++c) {
                const ChannelState& channel = channels[c];
                if (channel.muted || channel.voices == nullptr || local >= rows[c].size()
                    || rows[c][local] == 0)
                    continue;
                playStep(channel, step);
            }
        }
        for (const SongAudioClip& clip : current_->audioClips)
            if (clip.startStep == step)
                if (const ChannelState* channel = clipChannel(clip))
                    channel->voices->sampler.play(channel->sample.get(), audioClipOptions(0));
        return;
    }
    const auto index = static_cast<std::size_t>(step % current_->stepCount);
    for (const ChannelState& channel : current_->channels) {
        if (channel.muted || channel.voices == nullptr || index >= channel.steps.size()
            || channel.steps[index] == 0)
            continue;
        // Volume and pan are applied to the whole channel in mix(), live.
        playStep(channel, step);
    }
}

const ChannelState* StepSequencer::clipChannel(const SongAudioClip& clip) const noexcept
{
    if (current_ == nullptr || clip.channel < 0
        || static_cast<std::size_t>(clip.channel) >= current_->channels.size())
        return nullptr;
    const ChannelState& channel = current_->channels[static_cast<std::size_t>(clip.channel)];
    return channel.muted || channel.voices == nullptr || channel.sample == nullptr ? nullptr : &channel;
}

void StepSequencer::resumeAt(double ticks, double framesPerTick) noexcept
{
    if (current_ == nullptr)
        return;
    for (const ChannelState& channel : current_->channels)
        if (channel.voices != nullptr)
            channel.voices->releaseHeld();
    if (!current_->song || current_->ticksPerStep <= 0 || !(framesPerTick > 0.0))
        return;
    // Whatever the clips' channels were playing belongs to the old position.
    for (const SongAudioClip& clip : current_->audioClips)
        if (const ChannelState* channel = clipChannel(clip))
            channel->voices->stopAll();
    for (const SongAudioClip& clip : current_->audioClips) {
        const ChannelState* channel = clipChannel(clip);
        if (channel == nullptr)
            continue;
        const double start = static_cast<double>(clip.startStep) * current_->ticksPerStep;
        if (start >= ticks)
            continue; // starts later, or right here: triggerStep() starts it
        const auto offset = static_cast<std::int64_t>(std::floor((ticks - start) * framesPerTick));
        channel->voices->sampler.play(
            channel->sample.get(), audioClipOptions(offset)); // ignored past the end
    }
}

void StepSequencer::render(int startFrame, int numFrames) noexcept
{
    ++pass_;
    const auto play = [&](const ChannelState& channel) {
        ChannelVoices* voices = channel.voices.get();
        if (voices == nullptr || voices->renderedPass == pass_ || voices->activeVoices() == 0)
            return;
        voices->renderedPass = pass_;
        float* const buffer[2] = {voices->left.data(), voices->right.data()};
        if (voices->sampler.activeVoices() > 0) {
            voices->sampler.setPitch(pitch_);
            voices->sampler.render(buffer, 2, startFrame, numFrames);
        }
        if (voices->synth.activeVoices() > 0) {
            voices->synth.setSampleRate(sampleRate_);
            voices->synth.setPitch(pitch_);
            voices->synth.render(channel.synth, buffer, 2, startFrame, numFrames);
        }
        voices->written = true;
    };
    if (current_ != nullptr)
        for (const ChannelState& channel : current_->channels)
            play(channel);
    // Removed channels: their last notes fade out (stopAll() only starts a
    // fade once per note) and are rendered from the old state until silent.
    exchange_.forEachDraining([&](const SequencerState& old) {
        for (const ChannelState& channel : old.channels) {
            ChannelVoices* voices = channel.voices.get();
            if (voices == nullptr || voices->activeVoices() == 0 || inCurrent(voices))
                continue;
            voices->stopAll();
            play(channel);
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
            channel.voices->stopAll();
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
        if (voices != nullptr && !inCurrent(voices) && voices->activeVoices() > 0)
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
