// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/SongExport.hpp"

#include "vloom/engine/Engine.hpp"
#include "vloom/engine/sample/SampleBuffer.hpp"
#include "vloom/engine/sequencer/StepSequencer.hpp"
#include "vloom/engine/time/MusicalTime.hpp"
#include "vloom/engine/time/Transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

namespace vloom {

namespace {

double clampTempo(double tempo) { return std::clamp(tempo, Transport::kMinBpm, Transport::kMaxBpm); }

/// A clip whose ticks stay within StepSequencer::kMaxTicks (others never play).
bool inRange(std::int64_t startStep, std::int64_t lengthSteps, std::int64_t ticksPerStep)
{
    const std::int64_t maxSteps = StepSequencer::kMaxTicks / ticksPerStep;
    return startStep >= 0 && startStep <= maxSteps && lengthSteps >= 0 && lengthSteps <= maxSteps;
}

} // namespace

std::int64_t songEndTicks(const SequencerState& song, double tempo)
{
    if (song.ticksPerStep <= 0 || !std::isfinite(tempo))
        return 0;
    const std::int64_t ticksPerStep = song.ticksPerStep;
    if (!song.song)
        // int x int: at most about 4.6e18, inside int64.
        return song.stepCount > 0 ? static_cast<std::int64_t>(song.stepCount) * ticksPerStep : 0;

    std::int64_t end = 0;
    for (const SongPatternClip& clip : song.patternClips) {
        if (clip.pattern < 0 || static_cast<std::size_t>(clip.pattern) >= song.patterns.size()
            || clip.lengthSteps <= 0 || !inRange(clip.startStep, clip.lengthSteps, ticksPerStep))
            continue;
        end = std::max(end, (clip.startStep + clip.lengthSteps) * ticksPerStep);
    }
    // An audio clip lasts as long as its sample, whatever the tempo: in
    // ticks that is longer at a faster tempo.
    const double ticksPerSecond = clampTempo(tempo) / 60.0 * static_cast<double>(kPpq);
    for (const SongAudioClip& clip : song.audioClips) {
        if (clip.channel < 0 || static_cast<std::size_t>(clip.channel) >= song.channels.size()
            || !inRange(clip.startStep, 0, ticksPerStep))
            continue;
        const ChannelState& channel = song.channels[static_cast<std::size_t>(clip.channel)];
        if (channel.sample == nullptr || channel.sample->empty() || !(channel.sample->sampleRate() > 0.0))
            continue;
        const double seconds = static_cast<double>(channel.sample->frames()) / channel.sample->sampleRate();
        // Capped before the conversion: a sample claiming a tiny rate would
        // otherwise last longer than int64 ticks hold (undefined behaviour).
        const double ticks = std::ceil(seconds * ticksPerSecond);
        const auto length
            = static_cast<std::int64_t>(std::min(ticks, static_cast<double>(StepSequencer::kMaxTicks)));
        end = std::max(end, clip.startStep * ticksPerStep + length);
    }
    return end;
}

std::unique_ptr<SequencerState> copyWithOwnVoices(const SequencerState& state)
{
    auto copy = std::make_unique<SequencerState>(state);
    for (ChannelState& channel : copy->channels)
        if (channel.voices != nullptr)
            channel.voices = std::make_shared<ChannelVoices>(channel.voices->sampler.maxVoices());
    return copy;
}

SongExportResult exportSong(const std::string& path, const SequencerState& song, const MixerState* mixer,
    double sampleRate, const SongExportOptions& options, const SongExportProgress& progress)
{
    SongExportResult result;
    const auto fail = [&result](std::string message) {
        result.error = std::move(message);
        return result;
    };

    if (!(sampleRate >= Engine::kMinSampleRate && sampleRate <= Engine::kMaxSampleRate))
        return fail("The sample rate must be from 8 to 768 kHz");
    if (options.channels != 1 && options.channels != 2)
        return fail("Only mono and stereo files can be exported");
    if (!std::isfinite(options.tempo))
        return fail("The tempo is not a number");
    for (const ChannelState& channel : song.channels)
        if (channel.sample != nullptr && channel.sample->sampleRate() != sampleRate)
            return fail(
                "A sample is loaded at another rate than the export's: load it for the export rate first");

    const double tempo = clampTempo(options.tempo);
    const std::int64_t start = std::max<std::int64_t>(options.startTicks, 0);
    const std::int64_t end = options.endTicks < 0 ? songEndTicks(song, tempo) : options.endTicks;
    if (end <= start)
        return fail("There is nothing to export");
    if (end > StepSequencer::kMaxTicks)
        return fail("The range to export is too long");
    const double framesPerTick = sampleRate * 60.0 / (tempo * static_cast<double>(kPpq));
    const double body = std::ceil(static_cast<double>(end - start) * framesPerTick);
    const double tailSeconds
        = std::isfinite(options.maxTailSeconds) ? std::clamp(options.maxTailSeconds, 0.0, 600.0) : 0.0;
    const double tail = std::floor(tailSeconds * sampleRate);
    // Refused before rendering: a song that could not fit a WAV file (4 GiB,
    // header included), even with its whole possible tail, would otherwise
    // only fail once the writer is full, after a long render.
    constexpr double kHeaderBytes = 64.0; // more than either layout's header
    const double wavFrames
        = (4294967295.0 - kHeaderBytes) / (options.channels * wavBytesPerSample(options.format));
    if (!(body + tail < wavFrames))
        return fail("The song is too long for a WAV file (4 GiB)");
    const auto bodyFrames = static_cast<std::int64_t>(body);
    const auto tailFrames = static_cast<std::int64_t>(tail);
    const auto silentEnough = static_cast<std::int64_t>(0.05 * sampleRate);
    const float threshold
        = std::isfinite(options.silenceThreshold) ? std::max(options.silenceThreshold, 0.0f) : 0.0f;

    // An engine of its own (with its own voices): the live one never notices.
    auto engine = std::make_unique<Engine>();
    const int blockSize = std::clamp(options.blockSize, 1, kMaxBlockFrames);
    engine->prepare(sampleRate, blockSize);
    engine->setMetronomeEnabled(false);
    engine->setTempo(tempo);
    engine->setMasterGain(options.masterGain);
    engine->setMasterPan(options.masterPan);
    engine->setMasterPitch(options.masterPitchCents);
    engine->setSequencerState(copyWithOwnVoices(song));
    if (mixer != nullptr)
        engine->setMixerState(std::make_unique<MixerState>(*mixer));
    engine->locate(static_cast<double>(start));
    engine->play();

    WavFileWriter writer;
    WavFileWriter::Spec spec;
    spec.channels = options.channels;
    spec.sampleRate = static_cast<int>(std::lround(sampleRate));
    spec.format = options.format;
    spec.dither = options.dither;
    if (!writer.open(path, spec, &result.error))
        return result;

    const auto width = static_cast<std::size_t>(options.channels);
    const auto length = static_cast<std::size_t>(blockSize);
    std::vector<float> planar(width * length);
    std::array<float*, 2> ptrs{planar.data(), planar.data() + (width > 1 ? length : 0)};
    std::vector<float> interleaved(width * length);

    // Renders `frames` and writes them; the block's peak in `peak`.
    const auto renderBlock = [&](int frames, float& peak) {
        engine->process(ptrs.data(), options.channels, frames);
        peak = 0.0f;
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames); ++i)
            for (std::size_t ch = 0; ch < width; ++ch) {
                const float s = ptrs[ch][i];
                interleaved[i * width + ch] = s;
                peak = std::max(peak, std::abs(s));
            }
        return writer.write(
            std::span<const float>(interleaved.data(), static_cast<std::size_t>(frames) * width),
            &result.error);
    };
    const auto keepGoing = [&](double done) {
        if (!progress || progress(std::clamp(done, 0.0, 1.0)))
            return true;
        result.cancelled = true;
        result.error = "Export cancelled";
        return false; // the writer's destructor removes the unfinished file
    };

    // The song itself.
    float peak = 0.0f;
    for (std::int64_t done = 0; done < bodyFrames;) {
        const auto frames = static_cast<int>(std::min<std::int64_t>(blockSize, bodyFrames - done));
        if (!renderBlock(frames, peak))
            return result;
        done += frames;
        if (!keepGoing(0.9 * static_cast<double>(done) / static_cast<double>(bodyFrames)))
            return result;
    }
    // Then until the sound has died away.
    std::int64_t silent = 0;
    for (std::int64_t done = 0; done < tailFrames && silent < silentEnough;) {
        const auto frames = static_cast<int>(std::min<std::int64_t>(blockSize, tailFrames - done));
        if (!renderBlock(frames, peak))
            return result;
        done += frames;
        silent = peak <= threshold ? silent + frames : 0;
        if (!keepGoing(0.9 + 0.1 * static_cast<double>(done) / static_cast<double>(tailFrames)))
            return result;
    }

    if (!writer.finish(&result.error))
        return result;
    if (progress)
        progress(1.0);
    result.ok = true;
    result.frames = writer.framesWritten();
    return result;
}

} // namespace vloom
