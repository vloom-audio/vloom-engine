// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/io/MidiMessage.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vloom {

struct MidiInputInfo {
    std::string name;
};

/// A MIDI input port (a keyboard, a pad controller), through each OS's own
/// MIDI API: WinMM on Windows, CoreMIDI on macOS, the ALSA sequencer on Linux
/// (which also sees what PipeWire and JACK bridge to it). ADR 0015.
///
/// Open one MidiInput per port. The callback runs on a thread of the OS's
/// (Windows, macOS) or of the input's own (Linux), one message at a time;
/// keep it short (Engine::playMidi() is made for it). After stop() or close()
/// returns it is not called again. System exclusive messages are not
/// passed on, nor clock and other real-time bytes.
///
/// Opening a port makes no sound and claims nothing another program needs,
/// except on Windows, where WinMM gives a port to one program at a time.
class MidiInput {
public:
    using Callback = std::function<void(const MidiMessage& message)>;

    MidiInput();
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    /// The input ports there are now, in the order open() counts them.
    /// Empty where the OS has no MIDI (or refused).
    [[nodiscard]] static std::vector<MidiInputInfo> listDevices();

    /// Opens port `index` of listDevices() (but does not start it). On
    /// failure returns false and fills `error`. Closes a port opened before.
    bool open(int index, Callback callback, std::string* error = nullptr);
    /// Messages start to arrive.
    bool start(std::string* error = nullptr);
    void stop();
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] std::string deviceName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vloom
