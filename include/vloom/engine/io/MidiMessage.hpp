// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <algorithm>
#include <cstdint>

namespace vloom {

/// One short MIDI message (MIDI 1.0): a status byte and up to two data
/// bytes, as a keyboard sends them. System exclusive messages do not fit and
/// are not passed on.
struct MidiMessage {
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

/// What a MidiMessage means to the engine.
struct MidiEvent {
    enum class Kind : std::uint8_t {
        None, ///< nothing the engine plays: system messages, program changes, malformed bytes
        NoteOn,
        NoteOff,
        PitchBend, ///< value -1 .. +1, 0 = centre
        Controller, ///< `number` 0 .. 127, value 0 .. 1
        Pressure, ///< channel pressure (aftertouch), value 0 .. 1
    };
    Kind kind = Kind::None;
    int channel = 0; ///< the MIDI channel, 0 .. 15
    int key = 0; ///< NoteOn, NoteOff: 0 .. 127
    int number = 0; ///< Controller: which one
    float value = 0.0f; ///< NoteOn: the velocity 0 .. 1; see Kind for the others
};

/// Reads one message. A note-on with velocity 0 is a note-off, as MIDI has
/// it. Polyphonic aftertouch, program changes and system messages give
/// Kind::None; so do data bytes over 127.
[[nodiscard]] constexpr MidiEvent decodeMidi(const MidiMessage& message) noexcept
{
    MidiEvent event;
    if (message.status < 0x80 || message.status >= 0xF0 || message.data1 > 127 || message.data2 > 127)
        return event;
    event.channel = message.status & 0x0F;
    const int a = message.data1;
    const int b = message.data2;
    switch (message.status & 0xF0) {
    case 0x80:
        event.kind = MidiEvent::Kind::NoteOff;
        event.key = a;
        break;
    case 0x90:
        event.kind = b == 0 ? MidiEvent::Kind::NoteOff : MidiEvent::Kind::NoteOn;
        event.key = a;
        event.value = static_cast<float>(b) / 127.0f;
        break;
    case 0xB0:
        event.kind = MidiEvent::Kind::Controller;
        event.number = a;
        event.value = static_cast<float>(b) / 127.0f;
        break;
    case 0xD0:
        event.kind = MidiEvent::Kind::Pressure;
        event.value = static_cast<float>(a) / 127.0f;
        break;
    case 0xE0: {
        // 14 bits, 8192 the centre; both ends reach exactly -1 and +1.
        const int bend = (a | (b << 7)) - 8192;
        event.kind = MidiEvent::Kind::PitchBend;
        event.value = std::clamp(static_cast<float>(bend) / (bend > 0 ? 8191.0f : 8192.0f), -1.0f, 1.0f);
        break;
    }
    default: // 0xA0 polyphonic aftertouch, 0xC0 program change
        break;
    }
    return event;
}

} // namespace vloom
