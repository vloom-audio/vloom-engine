// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/record/AudioTake.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vloom {

bool AudioTake::open(const std::string& path, int sampleRate, std::string* error)
{
    marks_.clear();
    written_ = 0;
    WavFileWriter::Spec spec;
    spec.channels = 2;
    spec.sampleRate = sampleRate;
    spec.format = WavFormat::Float32;
    open_ = writer_.open(path, spec, error);
    return open_;
}

bool AudioTake::collect(Engine& engine, std::string* error)
{
    // The audio first, the marks after: the audio thread pushes a mark before
    // the frames it marks, so every frame taken here has its mark taken too.
    const auto takeMarks = [&] {
        std::array<AudioRecordMark, 64> marks{};
        for (std::size_t got = 0; (got = engine.takeAudioMarks(marks)) > 0;)
            marks_.insert(marks_.end(), marks.begin(), marks.begin() + static_cast<std::ptrdiff_t>(got));
    };
    if (!open_) {
        // Nothing to write to: still empty the engine, so it does not lose
        // frames later on.
        while (engine.takeRecordedAudio(buffer_) > 0) { }
        takeMarks();
        if (error != nullptr)
            *error = "the take is not open";
        return false;
    }
    for (;;) {
        const std::size_t frames = engine.takeRecordedAudio(buffer_);
        if (frames == 0)
            break;
        if (!writer_.write(std::span<const float>(buffer_.data(), 2 * frames), error)) {
            open_ = false;
            takeMarks();
            return false;
        }
        written_ += static_cast<std::int64_t>(frames);
    }
    takeMarks();
    return true;
}

bool AudioTake::finish(Engine& engine, std::string* error)
{
    if (!collect(engine, error))
        return false;
    open_ = false;
    return writer_.finish(error);
}

std::vector<AudioTakeSegment> AudioTake::segments() const
{
    // Where each mark starts a stretch of the file. The input comes back
    // `shift` frames after the engine rendered what was played along with:
    // a take's first frames go before its mark's position, and a jump of
    // the song shows in the file that many frames after the engine made it.
    struct Bound {
        std::int64_t frame;
        double ticks;
        double ticksPerFrame;
        std::int32_t pass;
        bool newTake;
    };
    std::vector<Bound> bounds;
    std::int64_t shift = 0;
    for (const AudioRecordMark& mark : marks_) {
        // Frames lost restart the count where they were: a mark earlier than
        // the last ones overrides them.
        // A punch mark is where the player heard that very frame: not moved.
        const std::int64_t at = mark.kind == AudioRecordMark::Kind::Jump ? mark.frame + shift : mark.frame;
        bool takeStarts = false;
        while (!bounds.empty() && bounds.back().frame >= at) {
            takeStarts = takeStarts || bounds.back().newTake;
            bounds.pop_back();
        }
        if (mark.kind == AudioRecordMark::Kind::Punch) {
            // Punch recording (ADR 0017): it replaces the take's start at the
            // same frame, and is the take's first stretch then.
            bounds.push_back(Bound{at, mark.ticks, mark.ticksPerFrame, mark.pass, takeStarts});
        } else if (mark.kind == AudioRecordMark::Kind::Start) {
            shift = std::max(mark.shiftFrames, 0);
            bounds.push_back(Bound{mark.frame, mark.ticks - static_cast<double>(shift) * mark.ticksPerFrame,
                mark.ticksPerFrame, mark.pass, true});
        } else {
            bounds.push_back(Bound{at, mark.ticks, mark.ticksPerFrame, mark.pass, false});
        }
    }
    std::vector<AudioTakeSegment> segments;
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const Bound& bound = bounds[i];
        const std::int64_t end = std::min(i + 1 < bounds.size() ? bounds[i + 1].frame : written_, written_);
        AudioTakeSegment segment;
        segment.firstFrame = bound.frame;
        segment.startTicks = bound.ticks;
        segment.ticksPerFrame = bound.ticksPerFrame;
        segment.pass = bound.pass;
        segment.newTake = bound.newTake;
        // What was heard before the song's start is left out.
        if (segment.startTicks < 0.0 && segment.ticksPerFrame > 0.0) {
            const auto skip
                = static_cast<std::int64_t>(std::ceil(-segment.startTicks / segment.ticksPerFrame));
            segment.firstFrame += skip;
            // Rounding leaves a hair either side of 0: it is the start.
            segment.startTicks
                = std::max(segment.startTicks + static_cast<double>(skip) * segment.ticksPerFrame, 0.0);
            if (segment.startTicks < 1e-9)
                segment.startTicks = 0.0;
        }
        segment.frames = end - segment.firstFrame;
        if (segment.frames > 0)
            segments.push_back(segment);
    }
    return segments;
}

} // namespace vloom
