// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/sequencer/PluginNotes.hpp>

#include <catch2/catch_test_macros.hpp>

using vloom::NoteEvent;
using vloom::PluginNotes;

TEST_CASE("PluginNotes holds keys until their release tick", "[sequencer][plugin]")
{
    PluginNotes notes;
    notes.noteOn(10, 60, 0.5f, 240);
    notes.noteOn(20, 64, 1.0f, 480);
    CHECK(notes.held() == 2);
    CHECK(notes.nextRelease() == 240);
    notes.release(239, 30);
    CHECK(notes.held() == 2);
    notes.release(240, 40);
    CHECK(notes.held() == 1);
    CHECK(notes.nextRelease() == 480);

    const auto events = notes.events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].type == NoteEvent::Type::On);
    CHECK(events[0].frame == 10);
    CHECK(events[0].key == 60);
    CHECK(events[0].velocity == 0.5f);
    CHECK(events[2].type == NoteEvent::Type::Off);
    CHECK(events[2].frame == 40);
    CHECK(events[2].key == 60);

    notes.clearEvents();
    CHECK(notes.events().empty());
    CHECK(notes.active() == 1);
    notes.releaseHeld(5);
    CHECK(notes.held() == 0);
    CHECK(notes.nextRelease() == PluginNotes::kNoRelease);
    CHECK(notes.active() == 1); // the off still has to reach the plugin
    notes.clearEvents();
    CHECK(notes.active() == 0);
}

TEST_CASE("PluginNotes ends a key before starting it again", "[sequencer][plugin]")
{
    PluginNotes notes;
    notes.noteOn(0, 60, 1.0f, 1000);
    notes.noteOn(8, 60, 1.0f, 2000);
    CHECK(notes.held() == 1);
    CHECK(notes.nextRelease() == 2000);
    const auto events = notes.events();
    REQUIRE(events.size() == 3);
    CHECK(events[1].type == NoteEvent::Type::Off);
    CHECK(events[2].type == NoteEvent::Type::On);
    CHECK(events[2].frame == 8);
    notes.noteOn(0, 128, 1.0f, 10); // out of range
    notes.noteOn(0, -1, 1.0f, 10);
    CHECK(notes.events().size() == 3);
}

TEST_CASE("PluginNotes always has room to end a held key", "[sequencer][plugin]")
{
    PluginNotes notes;
    for (int i = 0; i < 1000; ++i)
        notes.noteOn(i, i % 128, 1.0f, 100 + i);
    CHECK(notes.events().size() <= PluginNotes::kMaxEvents);
    const int held = notes.held();
    REQUIRE(held > 0);
    const std::size_t before = notes.events().size();
    notes.releaseHeld(999);
    CHECK(notes.events().size() == before + static_cast<std::size_t>(held)); // every off made it
    CHECK(notes.held() == 0);
}
