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

/// The first playable note of `row` (sorted by start) starting at or after
/// `from`, or null.
const Note* firstPlayable(const std::vector<Note>& row, std::int64_t from, std::int64_t span) noexcept
{
    auto it = std::lower_bound(
        row.begin(), row.end(), from, [](const Note& note, std::int64_t at) { return note.start < at; });
    for (; it != row.end() && it->start < span; ++it)
        if (playable(*it, span))
            return &*it;
    return nullptr;
}

/// Calls `play(note)` for the playable notes of `row` starting exactly at `at`.
template <typename Play>
void notesAt(const std::vector<Note>& row, std::int64_t at, std::int64_t span, Play&& play)
{
    auto it = std::lower_bound(
        row.begin(), row.end(), at, [](const Note& note, std::int64_t t) { return note.start < t; });
    for (; it != row.end() && it->start == at; ++it)
        if (playable(*it, span))
            play(*it);
}

void sortNotes(std::vector<Note>& notes)
{
    std::stable_sort(
        notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
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

void StepSequencer::publish(std::unique_ptr<SequencerState> state)
{
    // Sorted once here, on the control thread, so the audio thread finds the
    // next note by binary search.
    if (state != nullptr) {
        for (ChannelState& channel : state->channels)
            sortNotes(channel.notes);
        for (SongPattern& pattern : state->patterns)
            for (std::vector<Note>& row : pattern.notes)
                sortNotes(row);
    }
    exchange_.publish(std::unique_ptr<const SequencerState>(std::move(state)));
}

bool StepSequencer::clipInRange(std::int64_t startStep, std::int64_t lengthSteps) const noexcept
{
    // Keeps every tick computed from a clip (start + span + note length)
    // far inside int64.
    const std::int64_t maxSteps = kMaxTicks / current_->ticksPerStep;
    return startStep >= 0 && startStep <= maxSteps && lengthSteps > 0 && lengthSteps <= maxSteps;
}

std::int64_t StepSequencer::nextNoteStart(std::int64_t from) const noexcept
{
    if (current_ == nullptr || current_->stepCount <= 0 || current_->ticksPerStep <= 0 || from < 0)
        return kNoEvent;
    const std::int64_t ticksPerStep = current_->ticksPerStep;
    const auto& channels = current_->channels;
    std::int64_t next = kNoEvent;
    if (current_->song) {
        for (const SongPatternClip& clip : current_->patternClips) {
            if (clip.pattern < 0 || static_cast<std::size_t>(clip.pattern) >= current_->patterns.size()
                || !clipInRange(clip.startStep, clip.lengthSteps))
                continue;
            const std::int64_t start = clip.startStep * ticksPerStep;
            const std::int64_t span = clip.lengthSteps * ticksPerStep;
            if (start + span <= from || start >= next)
                continue; // over already, or nothing in it can come first
            const auto& rows = current_->patterns[static_cast<std::size_t>(clip.pattern)].notes;
            for (std::size_t c = 0; c < channels.size() && c < rows.size(); ++c)
                if (const Note* note = firstPlayable(rows[c], std::max<std::int64_t>(from - start, 0), span))
                    next = std::min(next, start + note->start);
        }
        return next;
    }
    // Pattern mode: the pattern loops, so each note comes round once per period.
    const std::int64_t period = static_cast<std::int64_t>(current_->stepCount) * ticksPerStep;
    const std::int64_t loop = from / period;
    const std::int64_t local = from - loop * period;
    for (const ChannelState& channel : channels) {
        if (const Note* note = firstPlayable(channel.notes, local, period))
            next = std::min(next, loop * period + note->start);
        else if (const Note* first = firstPlayable(channel.notes, 0, period))
            next = std::min(next, (loop + 1) * period + first->start);
    }
    return next;
}

void StepSequencer::startNotes(std::int64_t tick) noexcept
{
    if (current_ == nullptr || current_->stepCount <= 0 || current_->ticksPerStep <= 0 || tick < 0)
        return;
    const std::int64_t ticksPerStep = current_->ticksPerStep;
    const auto& channels = current_->channels;
    // Held at most kMaxTicks, so the end stays far inside int64.
    const auto play = [this, tick](const ChannelState& channel, const Note& note) {
        playNote(channel, note, tick + std::min(note.length, kMaxTicks));
    };
    if (current_->song) {
        for (const SongPatternClip& clip : current_->patternClips) {
            if (clip.pattern < 0 || static_cast<std::size_t>(clip.pattern) >= current_->patterns.size()
                || !clipInRange(clip.startStep, clip.lengthSteps))
                continue;
            const std::int64_t start = clip.startStep * ticksPerStep;
            const std::int64_t span = clip.lengthSteps * ticksPerStep;
            if (tick < start || tick >= start + span)
                continue;
            const auto& rows = current_->patterns[static_cast<std::size_t>(clip.pattern)].notes;
            for (std::size_t c = 0; c < channels.size() && c < rows.size(); ++c)
                notesAt(rows[c], tick - start, span, [&](const Note& note) { play(channels[c], note); });
        }
        return;
    }
    const std::int64_t period = static_cast<std::int64_t>(current_->stepCount) * ticksPerStep;
    for (const ChannelState& channel : channels)
        notesAt(channel.notes, tick % period, period, [&](const Note& note) { play(channel, note); });
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

void StepSequencer::resetVoices() noexcept
{
    const auto reset = [](const SequencerState& state) {
        for (const ChannelState& channel : state.channels)
            if (channel.voices != nullptr) {
                channel.voices->sampler.reset();
                channel.voices->synth.reset();
            }
    };
    if (const SequencerState* state = exchange_.current())
        reset(*state);
    exchange_.forEachDraining(reset);
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
            if (clip.pattern < 0 || static_cast<std::size_t>(clip.pattern) >= current_->patterns.size()
                || current_->ticksPerStep <= 0 || !clipInRange(clip.startStep, clip.lengthSteps)
                || step < clip.startStep || step >= clip.startStep + clip.lengthSteps)
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
        || static_cast<std::size_t>(clip.channel) >= current_->channels.size() || current_->ticksPerStep <= 0
        || clip.startStep < 0 || clip.startStep > kMaxTicks / current_->ticksPerStep)
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
        // Starts later, or less than a frame before `ticks`: the engine's
        // step at the clip's start still fires (it lands on this block's
        // first sample, by the scheduler's rule), so triggerStep() starts it.
        constexpr double kEpsilon = 1e-6; // as in Engine::processBlock
        if ((ticks - start) * framesPerTick < 1.0 - kEpsilon)
            continue;
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
