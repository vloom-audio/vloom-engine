// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/io/MidiMessage.hpp"
#include "vloom/engine/sequencer/SequencerState.hpp"

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace vloom {

/// A MIDI message played live while recording (Engine::takeRecordedMidi()):
/// where in the song the player heard it, and for which channel.
struct RecordedMidi {
    /// The song position (ticks from its start) the player heard when they
    /// played it: the engine's position less the latency of what they hear
    /// (Engine::setRecordLatency(), Engine::latencyFrames()).
    double ticks = 0.0;
    /// The loop's passes before this one since the song was started (0
    /// without a loop): a note let go in a later pass than it started in
    /// was held over the loop's end.
    std::int32_t pass = 0;
    std::int32_t channel = 0; ///< index into SequencerState::channels
    MidiMessage message;
};

/// A note of a take: from its key going down to its going up.
struct RecordedNote {
    int channel = 0; ///< index into SequencerState::channels
    std::int64_t start = 0; ///< ticks from the song start
    std::int64_t length = 1; ///< ticks, at least 1
    int key = Note::kMiddleC;
    float velocity = 0.0f; ///< 0 .. 1
    int midiChannel = 0; ///< 0 .. 15 (an MPE controller plays each note on its own)
};

/// A pitch bend, controller or pressure of a take.
struct RecordedControl {
    int channel = 0; ///< index into SequencerState::channels
    ControlPoint point; ///< `start` in ticks from the song start
};

/// Puts a recording together (ADR 0015): the messages taken from the engine
/// become notes and controls, ready to be written into a pattern. Control
/// thread; feed it in the order the engine gave them.
///
/// A key played twice without being let go ends its first note where the
/// second starts. Within a loop, a note held over the loop's end ends there.
/// The notes still held when the recording stops are ended by finish().
class MidiTake {
public:
    /// The loop the take is played in, [startTicks, endTicks); none when
    /// endTicks <= startTicks (the default).
    void setLoop(double startTicks, double endTicks) noexcept;
    void add(const RecordedMidi& event);
    /// Ends the notes still held at `ticks` in loop pass `pass` (where the
    /// song stopped, or the recording was switched off).
    void finish(double ticks, std::int32_t pass = 0);
    /// Forgets everything (a new take); the loop stays.
    void clear();

    /// The finished notes, in the order they were let go.
    [[nodiscard]] const std::vector<RecordedNote>& notes() const noexcept { return notes_; }
    [[nodiscard]] const std::vector<RecordedControl>& controls() const noexcept { return controls_; }
    /// Keys still down.
    [[nodiscard]] std::size_t held() const noexcept { return held_.size(); }

private:
    struct Held {
        double start = 0.0;
        std::int32_t pass = 0;
        float velocity = 0.0f;
    };
    /// (channel, MIDI channel, key)
    using HeldKey = std::tuple<int, int, int>;
    void end(const HeldKey& key, const Held& held, double ticks, std::int32_t pass);

    double loopStart_ = 0.0;
    double loopEnd_ = 0.0;
    std::map<HeldKey, Held> held_;
    std::vector<RecordedNote> notes_;
    std::vector<RecordedControl> controls_;
};

/// Moves each note's start towards the nearest multiple of `grid` ticks by
/// `strength` (0 = not at all, 1 = onto it); lengths stay. A grid <= 0 or a
/// strength that is not a number changes nothing.
void quantize(std::vector<RecordedNote>& notes, std::int64_t grid, double strength = 1.0) noexcept;

/// The notes of channel `channel` as a pattern's notes (ChannelState::notes,
/// SongPattern::notes): starts from `fromTicks` on, wrapped into a pattern of
/// `patternTicks` that repeats (pattern mode: stepCount * ticksPerStep);
/// patternTicks <= 0: not wrapped. Earlier notes are left out. The MIDI
/// channel does not go into a pattern's note.
[[nodiscard]] std::vector<Note> patternNotes(
    const std::vector<RecordedNote>& notes, int channel, std::int64_t fromTicks, std::int64_t patternTicks);
/// The same for the controls (ChannelState::controls, SongPattern::controls).
[[nodiscard]] std::vector<ControlPoint> patternControls(const std::vector<RecordedControl>& controls,
    int channel, std::int64_t fromTicks, std::int64_t patternTicks);

} // namespace vloom
