// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sequencer/MidiTake.hpp"

#include <algorithm>
#include <cmath>

namespace vloom {

namespace {

std::int64_t tickOf(double ticks) noexcept
{
    if (!std::isfinite(ticks) || ticks <= 0.0)
        return 0;
    return static_cast<std::int64_t>(std::llround(std::min(ticks, 1e15)));
}

/// A start in a pattern of `patternTicks` that repeats.
std::int64_t wrapped(std::int64_t ticks, std::int64_t patternTicks) noexcept
{
    return patternTicks > 0 ? ticks % patternTicks : ticks;
}

} // namespace

void MidiTake::setLoop(double startTicks, double endTicks) noexcept
{
    const bool loop = std::isfinite(startTicks) && std::isfinite(endTicks) && endTicks > startTicks;
    loopStart_ = loop ? startTicks : 0.0;
    loopEnd_ = loop ? endTicks : 0.0;
}

void MidiTake::setPunch(double startTicks, double endTicks) noexcept
{
    const bool punch = std::isfinite(startTicks) && std::isfinite(endTicks) && endTicks > startTicks;
    punchStart_ = punch ? startTicks : 0.0;
    punchEnd_ = punch ? endTicks : 0.0;
}

void MidiTake::end(const HeldKey& key, const Held& held, double ticks, std::int32_t pass)
{
    // Let go in a later pass of the loop: it was held over the loop's end.
    if (loopEnd_ > loopStart_ && pass > held.pass)
        ticks = loopEnd_;
    // Held over the punch-out (in its pass or into a later one): ends there.
    if (punched() && (pass > held.pass || ticks > punchEnd_))
        ticks = std::min(ticks, punchEnd_);
    RecordedNote note;
    note.channel = std::get<0>(key);
    note.midiChannel = std::get<1>(key);
    note.key = std::get<2>(key);
    note.start = tickOf(held.start);
    note.length = std::max<std::int64_t>(tickOf(ticks) - note.start, 1);
    note.velocity = held.velocity;
    notes_.push_back(note);
}

void MidiTake::add(const RecordedMidi& event)
{
    const MidiEvent midi = decodeMidi(event.message);
    const bool inside = !punched() || (event.ticks >= punchStart_ && event.ticks < punchEnd_);
    switch (midi.kind) {
    case MidiEvent::Kind::NoteOn:
    case MidiEvent::Kind::NoteOff: {
        const HeldKey key{event.channel, midi.channel, midi.key};
        const auto it = held_.find(key);
        if (it != held_.end()) {
            end(key, it->second, event.ticks, event.pass);
            held_.erase(it);
        }
        if (midi.kind == MidiEvent::Kind::NoteOn && inside)
            held_[key] = Held{event.ticks, event.pass, midi.value};
        break;
    }
    case MidiEvent::Kind::PitchBend:
    case MidiEvent::Kind::Controller:
    case MidiEvent::Kind::Pressure: {
        if (!inside)
            break;
        RecordedControl control;
        control.channel = event.channel;
        control.point.start = tickOf(event.ticks);
        control.point.type = midi.kind == MidiEvent::Kind::PitchBend
            ? ControlEvent::Type::PitchBend
            : (midi.kind == MidiEvent::Kind::Pressure ? ControlEvent::Type::Pressure
                                                      : ControlEvent::Type::Controller);
        control.point.number = static_cast<std::int16_t>(midi.number);
        control.point.value = midi.value;
        control.point.channel = static_cast<std::int16_t>(midi.channel);
        controls_.push_back(control);
        break;
    }
    case MidiEvent::Kind::None:
        break;
    }
}

void MidiTake::finish(double ticks, std::int32_t pass)
{
    for (const auto& [key, held] : held_)
        end(key, held, ticks, pass);
    held_.clear();
}

void MidiTake::clear()
{
    held_.clear();
    notes_.clear();
    controls_.clear();
}

void quantize(std::vector<RecordedNote>& notes, std::int64_t grid, double strength) noexcept
{
    if (grid <= 0 || !std::isfinite(strength))
        return;
    strength = std::clamp(strength, 0.0, 1.0);
    for (RecordedNote& note : notes) {
        const std::int64_t nearest = (note.start + grid / 2) / grid * grid;
        const auto moved = static_cast<double>(nearest - note.start) * strength;
        note.start = std::max<std::int64_t>(note.start + static_cast<std::int64_t>(std::llround(moved)), 0);
    }
}

std::vector<Note> patternNotes(
    const std::vector<RecordedNote>& notes, int channel, std::int64_t fromTicks, std::int64_t patternTicks)
{
    std::vector<Note> out;
    for (const RecordedNote& recorded : notes) {
        if (recorded.channel != channel || recorded.start < fromTicks)
            continue;
        Note note;
        note.start = wrapped(recorded.start - fromTicks, patternTicks);
        note.length = std::max<std::int64_t>(recorded.length, 1);
        note.key = std::clamp(recorded.key, Note::kMinKey, Note::kMaxKey);
        note.velocity = std::clamp(recorded.velocity, 0.0f, 1.0f);
        out.push_back(note);
    }
    std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    return out;
}

std::vector<ControlPoint> patternControls(const std::vector<RecordedControl>& controls, int channel,
    std::int64_t fromTicks, std::int64_t patternTicks)
{
    std::vector<ControlPoint> out;
    for (const RecordedControl& recorded : controls) {
        if (recorded.channel != channel || recorded.point.start < fromTicks)
            continue;
        ControlPoint point = recorded.point;
        point.start = wrapped(point.start - fromTicks, patternTicks);
        out.push_back(point);
    }
    std::stable_sort(out.begin(), out.end(),
        [](const ControlPoint& a, const ControlPoint& b) { return a.start < b.start; });
    return out;
}

} // namespace vloom
