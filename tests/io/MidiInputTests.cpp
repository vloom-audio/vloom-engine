// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/io/MidiInput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

// Without MIDI hardware (CI): only what needs no port. Nothing here opens
// a port, so nothing is taken from another program either.

TEST_CASE("A MIDI input that is not open reports nothing and refuses to start", "[io][midi]")
{
    vloom::MidiInput input;
    CHECK_FALSE(input.isOpen());
    CHECK_FALSE(input.isRunning());
    CHECK(input.deviceName().empty());
    input.stop();
    input.close();
    std::string error;
    CHECK_FALSE(input.start(&error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Listing MIDI inputs works without any, and a port that is not there does not open", "[io][midi]")
{
    const auto devices = vloom::MidiInput::listDevices(); // may well be empty
    vloom::MidiInput input;
    std::string error;
    CHECK_FALSE(
        input.open(static_cast<int>(devices.size()) + 100, [](const vloom::MidiMessage&) { }, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(input.isOpen());
    error.clear();
    CHECK_FALSE(input.open(-1, [](const vloom::MidiMessage&) { }, &error));
    CHECK_FALSE(error.empty());
}
