// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/Bounce.hpp"

#include "render/SongRender.hpp"
#include "vloom/engine/sequencer/StepSequencer.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <span>

namespace vloom {

namespace {

/// The channels not in options.channels are muted (none when it is empty).
void keepOnly(SequencerState& state, const BounceOptions& options)
{
    if (options.channels.empty())
        return;
    for (std::size_t i = 0; i < state.channels.size(); ++i)
        if (std::find(options.channels.begin(), options.channels.end(), static_cast<int>(i))
            == options.channels.end())
            state.channels[i].muted = true;
}

BounceResult render(const SequencerState& song, std::int64_t startTicks, std::int64_t endTicks,
    const MixerState* mixer, double sampleRate, const BounceOptions& options,
    const SongExportProgress& progress)
{
    BounceResult result;
    result.lengthTicks = std::max<std::int64_t>(endTicks - startTicks, 0);
    for (const int channel : options.channels)
        if (channel < 0 || static_cast<std::size_t>(channel) >= song.channels.size()) {
            result.error = "A channel to bounce is not in the channel rack";
            return result;
        }

    SongExportOptions render;
    render.format = options.path.empty() ? WavFormat::Float32 : options.format;
    render.dither = options.dither;
    render.channels = 2;
    render.tempo = options.tempo;
    render.masterPitchCents = options.masterPitchCents;
    render.startTicks = startTicks;
    render.endTicks = endTicks;
    render.maxTailSeconds = options.maxTailSeconds;
    render.silenceThreshold = options.silenceThreshold;
    render.blockSize = options.blockSize;

    // Dry: no mixer state, so every insert plays at unity with no effect.
    // Inserts: the master's own chain, volume and pan are left out.
    std::unique_ptr<MixerState> inserts;
    const MixerState* through = nullptr;
    switch (options.mix) {
    case BounceMix::Dry:
        break;
    case BounceMix::Inserts:
        if (mixer != nullptr) {
            inserts = std::make_unique<MixerState>(*mixer);
            inserts->masterEffects.clear();
            through = inserts.get();
        }
        break;
    case BounceMix::Master:
        through = mixer;
        render.masterGain = options.masterGain;
        render.masterPan = options.masterPan;
        break;
    }

    WavFileWriter writer;
    detail::SongRenderOutput output;
    if (options.path.empty()) {
        // In memory: at most kMaxInMemoryBounceSeconds, tail included, so a
        // long range can't ask for gigabytes; running out of memory anyway
        // is an error in the result, never an exception out of the API.
        const auto maxFrames = static_cast<std::uint64_t>(kMaxInMemoryBounceSeconds * sampleRate);
        output.begin = [&, maxFrames](std::uint64_t bodyFrames, std::string* error) {
            result.bodyFrames = bodyFrames;
            if (bodyFrames > maxFrames) {
                if (error != nullptr)
                    *error = "Too long to bounce into memory; bounce to a file instead";
                return false;
            }
            try {
                result.audio.reserve(static_cast<std::size_t>(2 * bodyFrames));
            } catch (const std::exception&) {
                if (error != nullptr)
                    *error = "Not enough memory for the bounce";
                return false;
            }
            return true;
        };
        output.write = [&, maxFrames](std::span<const float> block, std::string* error) {
            if (result.audio.size() / 2 + block.size() / 2 > maxFrames) {
                if (error != nullptr)
                    *error = "Too long to bounce into memory; bounce to a file instead";
                return false;
            }
            try {
                result.audio.insert(result.audio.end(), block.begin(), block.end());
            } catch (const std::exception&) {
                if (error != nullptr)
                    *error = "Not enough memory for the bounce";
                return false;
            }
            return true;
        };
    } else {
        output.begin = [&](std::uint64_t bodyFrames, std::string* error) {
            result.bodyFrames = bodyFrames;
            WavFileWriter::Spec spec;
            spec.channels = 2;
            spec.sampleRate = static_cast<int>(std::lround(sampleRate));
            spec.format = options.format;
            spec.dither = options.dither;
            return writer.open(options.path, spec, error);
        };
        output.write
            = [&](std::span<const float> block, std::string* error) { return writer.write(block, error); };
        output.finish = [&](std::string* error) { return writer.finish(error); };
    }

    const SongExportResult rendered = detail::renderSong(song, through, sampleRate, render, output, progress);
    result.ok = rendered.ok;
    result.cancelled = rendered.cancelled;
    result.error = rendered.error;
    result.frames = rendered.frames;
    if (!result.ok)
        result.audio = {}; // what a failed or cancelled bounce got this far is not kept
    return result;
}

} // namespace

std::int64_t patternLengthTicks(const SequencerState& state, int pattern)
{
    if (state.ticksPerStep <= 0)
        return 0;
    const std::int64_t ticksPerStep = state.ticksPerStep;
    const std::int64_t rack = std::max<std::int64_t>(state.stepCount, 0) * ticksPerStep;
    if (pattern == kChannelRackPattern)
        return rack;
    if (pattern < 0 || static_cast<std::size_t>(pattern) >= state.patterns.size())
        return 0;
    const SongPattern& p = state.patterns[static_cast<std::size_t>(pattern)];
    if (p.stepCount > 0)
        return static_cast<std::int64_t>(p.stepCount) * ticksPerStep;
    std::size_t longest = 0;
    for (const auto& row : p.steps)
        longest = std::max(longest, row.size());
    return longest > 0 ? static_cast<std::int64_t>(longest) * ticksPerStep : rack;
}

BounceResult bouncePattern(const SequencerState& state, int pattern, const MixerState* mixer,
    double sampleRate, const BounceOptions& options, const SongExportProgress& progress)
{
    const std::int64_t length = patternLengthTicks(state, pattern);
    if (length <= 0) {
        BounceResult result;
        result.error = pattern == kChannelRackPattern
                || (pattern >= 0 && static_cast<std::size_t>(pattern) < state.patterns.size())
            ? "The pattern is empty"
            : "There is no such pattern";
        return result;
    }
    if (length > StepSequencer::kMaxTicks) {
        BounceResult result;
        result.error = "The pattern is too long";
        return result;
    }

    // The pattern alone in a playlist of its own: one clip from tick 0, as
    // long as the pattern. Song mode plays it like a clip in the song.
    SequencerState one = state;
    SongPattern played;
    if (pattern == kChannelRackPattern) {
        played.stepCount = state.stepCount;
        for (const ChannelState& channel : state.channels) {
            played.steps.push_back(channel.steps);
            played.notes.push_back(channel.notes);
            played.controls.push_back(channel.controls);
        }
    } else {
        played = state.patterns[static_cast<std::size_t>(pattern)];
    }
    one.song = true;
    one.patterns.assign(1, std::move(played));
    one.patternClips.assign(1, SongPatternClip{0, 0, length, 0});
    one.audioClips.clear();
    one.audioClipKeys.clear();
    keepOnly(one, options);
    return render(one, 0, length, mixer, sampleRate, options, progress);
}

BounceResult bounceRange(const SequencerState& song, std::int64_t startTicks, std::int64_t endTicks,
    const MixerState* mixer, double sampleRate, const BounceOptions& options,
    const SongExportProgress& progress)
{
    SequencerState part = song;
    part.song = true;
    part.audioClipKeys.clear();
    keepOnly(part, options);
    // As exportSong(): a negative end is the song's end.
    const std::int64_t end = endTicks < 0 ? songEndTicks(part, options.tempo) : endTicks;
    return render(part, std::max<std::int64_t>(startTicks, 0), end, mixer, sampleRate, options, progress);
}

} // namespace vloom
