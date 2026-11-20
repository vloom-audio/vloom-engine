// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/Engine.hpp"
#include "vloom/engine/render/WavWriter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vloom {

/// A stretch of a recorded file that plays at one place in the song: what a
/// playlist audio clip (SongAudioClip) of it would show.
struct AudioTakeSegment {
    std::int64_t firstFrame = 0; ///< in the file
    std::int64_t frames = 0;
    /// Where in the song (ticks from its start) firstFrame was heard. The
    /// tempo of the recording converts frames to ticks: ticksPerFrame.
    double startTicks = 0.0;
    double ticksPerFrame = 0.0;
    std::int32_t pass = 0; ///< the loop's pass it was recorded in (0 without a loop)
    bool newTake = false; ///< the first segment of a take (the song was started, or recording switched on)
};

/// Takes what the engine records (Engine::setAudioRecording()) and writes
/// it, as it comes, to a stereo 32-bit float WAV file, then says which
/// stretch of the file belongs where in the song (ADR 0016). Control thread.
///
/// Each stretch is moved back by the latency the engine reports, so it
/// lines up with what the player heard. In a loop, each pass is a stretch
/// of its own; so is each take, when the song is stopped and started again
/// while recording. With a punch range (Engine::setPunch(), ADR 0017) the
/// file holds only what was heard inside it, a stretch per pass, each
/// already placed where it was heard (AudioRecordMark::Kind::Punch).
class AudioTake {
public:
    AudioTake() = default;
    AudioTake(const AudioTake&) = delete;
    AudioTake& operator=(const AudioTake&) = delete;

    /// Starts the file at `path` (UTF-8), at the engine's `sampleRate`. It
    /// replaces `path` only in finish(). False with `error` on failure.
    bool open(const std::string& path, int sampleRate, std::string* error = nullptr);
    /// Moves what the engine recorded so far into the file. Call it often
    /// (a UI timer, every 50 ms or so): the engine keeps about 10 s.
    bool collect(Engine& engine, std::string* error = nullptr);
    /// Collects the rest and finishes the file.
    bool finish(Engine& engine, std::string* error = nullptr);

    [[nodiscard]] std::int64_t frames() const noexcept { return written_; }
    /// The stretches of the file, in the order they were recorded; up to
    /// date with what was collected.
    [[nodiscard]] std::vector<AudioTakeSegment> segments() const;
    /// The marks as the engine gave them.
    [[nodiscard]] const std::vector<AudioRecordMark>& marks() const noexcept { return marks_; }

private:
    WavFileWriter writer_;
    std::vector<float> buffer_ = std::vector<float>(2 * 8192, 0.0f); ///< frames on their way to the file
    std::vector<AudioRecordMark> marks_;
    std::int64_t written_ = 0;
    bool open_ = false;
};

} // namespace vloom
