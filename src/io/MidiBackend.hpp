// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// What each OS's MIDI input provides to MidiInput. One backend compiles per
// OS; on any other, there is none (no ports, open() fails).

#include "vloom/engine/io/MidiMessage.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vloom::io {

/// Called with each message as it arrives, on the backend's thread.
using MidiDeliver = std::function<void(const MidiMessage& message)>;

class MidiBackend {
public:
    virtual ~MidiBackend() = default;
    /// Opens port `index` (as midiInputNames() counts); `deliver` is set
    /// before. False and `error` filled on failure.
    virtual bool open(int index, std::string* error) = 0;
    virtual bool start(std::string* error) = 0;
    /// After it returns, `deliver` is not called again until start().
    virtual void stop() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

/// The input ports there are now; empty where there is no backend.
std::vector<std::string> midiInputNames();
/// Null where there is no backend.
std::unique_ptr<MidiBackend> makeMidiBackend(MidiDeliver deliver);

/// The bytes of a short message from a packed 32-bit word, status in the
/// lowest byte (WinMM's MIM_DATA). Real-time and other system bytes give a
/// message the caller drops (status 0).
inline MidiMessage unpackMidi(unsigned long packed) noexcept
{
    MidiMessage message;
    const auto status = static_cast<std::uint8_t>(packed & 0xFF);
    if (status < 0x80 || status >= 0xF0)
        return message;
    message.status = status;
    message.data1 = static_cast<std::uint8_t>((packed >> 8) & 0x7F);
    message.data2 = static_cast<std::uint8_t>((packed >> 16) & 0x7F);
    return message;
}

} // namespace vloom::io
