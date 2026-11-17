// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/MidiTake.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
// 120 BPM at 48 kHz: 25 frames a tick, 24000 frames a beat (kPpq = 960).

vloom::MidiMessage on(int key, int velocity = 127, int midiChannel = 0)
{
    return {static_cast<std::uint8_t>(0x90 | midiChannel), static_cast<std::uint8_t>(key),
        static_cast<std::uint8_t>(velocity)};
}

vloom::MidiMessage off(int key, int midiChannel = 0)
{
    return {static_cast<std::uint8_t>(0x80 | midiChannel), static_cast<std::uint8_t>(key), 0};
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
    engine.setTempo(120.0);
    engine.setMetronomeEnabled(false);
}

std::vector<float> render(vloom::Engine& engine, double seconds)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = 256;
    return vloom::renderInterleaved(engine, options);
}

float loudest(const std::vector<float>& audio, std::size_t fromFrame = 0)
{
    float peak = 0.0f;
    for (std::size_t i = 2 * fromFrame; i < audio.size(); ++i)
        peak = std::max(peak, std::abs(audio[i]));
    return peak;
}

std::unique_ptr<vloom::SequencerState> synthRack()
{
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState synth;
    synth.instrument = vloom::Instrument::Synth;
    synth.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(synth);
    return state;
}

std::vector<vloom::RecordedMidi> taken(vloom::Engine& engine)
{
    std::array<vloom::RecordedMidi, 64> events{};
    const std::size_t count = engine.takeRecordedMidi(events);
    return {events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count)};
}

} // namespace

TEST_CASE("MIDI messages are read the way MIDI means them", "[io][midi]")
{
    using Kind = vloom::MidiEvent::Kind;
    const vloom::MidiEvent noteOn = vloom::decodeMidi({0x93, 60, 127});
    CHECK(noteOn.kind == Kind::NoteOn);
    CHECK(noteOn.channel == 3);
    CHECK(noteOn.key == 60);
    CHECK(noteOn.value == 1.0f);
    CHECK(vloom::decodeMidi({0x90, 60, 0}).kind == Kind::NoteOff); // velocity 0 is a note-off
    CHECK(vloom::decodeMidi({0x8F, 61, 64}).kind == Kind::NoteOff);
    CHECK(vloom::decodeMidi({0x8F, 61, 64}).channel == 15);

    const vloom::MidiEvent wheel = vloom::decodeMidi({0xB0, 1, 127});
    CHECK(wheel.kind == Kind::Controller);
    CHECK(wheel.number == 1);
    CHECK(wheel.value == 1.0f);
    CHECK(vloom::decodeMidi({0xD2, 64, 0}).kind == Kind::Pressure);
    CHECK(vloom::decodeMidi({0xD2, 127, 0}).value == 1.0f);

    // Pitch bend: 14 bits, both ends exactly -1 and +1, the centre 0.
    CHECK(vloom::decodeMidi({0xE0, 0x00, 0x40}).value == 0.0f);
    CHECK(vloom::decodeMidi({0xE0, 0x7F, 0x7F}).value == 1.0f);
    CHECK(vloom::decodeMidi({0xE0, 0x00, 0x00}).value == -1.0f);
    CHECK(vloom::decodeMidi({0xE5, 0x00, 0x40}).kind == Kind::PitchBend);

    // What the engine does not play, and bytes that are not MIDI.
    CHECK(vloom::decodeMidi({0xA0, 60, 10}).kind == Kind::None); // polyphonic aftertouch
    CHECK(vloom::decodeMidi({0xC0, 5, 0}).kind == Kind::None); // program change
    CHECK(vloom::decodeMidi({0xF8, 0, 0}).kind == Kind::None); // clock
    CHECK(vloom::decodeMidi({0x40, 60, 100}).kind == Kind::None); // a data byte for a status
    CHECK(vloom::decodeMidi({0x90, 200, 100}).kind == Kind::None); // a data byte over 127
}

TEST_CASE("A key played live on a synth channel sounds until it is let go", "[sequencer][midi]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(synthRack());
    // Stopped: live notes play all the same.
    REQUIRE(engine.playMidi(0, on(69)));
    CHECK(loudest(render(engine, 0.2)) > 0.05f);
    REQUIRE(engine.playMidi(0, off(69)));
    const auto released = render(engine, 0.5); // the synth's release is 0.2 s
    CHECK(loudest(released, 20000) == 0.0f);

    // A stop or a loop does not end it: the key is still down.
    REQUIRE(engine.playMidi(0, on(64)));
    engine.setLoop(0.0, 960.0);
    engine.play();
    render(engine, 0.6); // loops back once
    engine.stop();
    CHECK(loudest(render(engine, 0.5), 20000) > 0.05f);
    REQUIRE(engine.playMidi(0, off(64)));
    CHECK(loudest(render(engine, 0.5), 20000) == 0.0f);

    // The same key on two MIDI channels is two notes: letting one go leaves the other.
    REQUIRE(engine.playMidi(0, on(60, 127, 1)));
    REQUIRE(engine.playMidi(0, on(60, 127, 2)));
    render(engine, 0.1);
    REQUIRE(engine.playMidi(0, off(60, 1)));
    CHECK(loudest(render(engine, 0.5), 20000) > 0.05f);
    REQUIRE(engine.playMidi(0, off(60, 2)));
    CHECK(loudest(render(engine, 0.5), 20000) == 0.0f);
}

TEST_CASE("A key played live on a sampler channel plays its sample at the key", "[sequencer][midi]")
{
    vloom::Engine engine;
    prepare(engine);
    auto sample = std::make_shared<vloom::SampleBuffer>(1, 96000, kRate);
    for (int i = 0; i < 96000; ++i)
        sample->channel(0)[i] = 0.5f;
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState channel;
    channel.sample = sample;
    channel.voices = std::make_shared<vloom::ChannelVoices>();
    channel.rootKey = 60;
    state->channels.push_back(channel);
    engine.setSequencerState(std::move(state));

    REQUIRE(engine.playMidi(0, on(72, 127)));
    CHECK(loudest(render(engine, 0.1)) > 0.3f);
    REQUIRE(engine.playMidi(0, off(72)));
    CHECK(loudest(render(engine, 0.1), 1000) == 0.0f); // released within 10 ms

    // Out of range, unknown or for a channel that is not there: nothing.
    CHECK(engine.playMidi(5, on(60)));
    CHECK(engine.playMidi(-1, on(60)));
    CHECK(engine.playMidi(0, {0xF8, 0, 0}));
    CHECK(loudest(render(engine, 0.1)) == 0.0f);
}

TEST_CASE("Recording takes the live notes at the song position they were played at", "[sequencer][midi]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(synthRack());

    // Not while stopped, nor while not recording.
    REQUIRE(engine.setRecording(true));
    CHECK(engine.isRecording());
    REQUIRE(engine.playMidi(0, on(60)));
    REQUIRE(engine.playMidi(0, off(60)));
    render(engine, 0.05);
    CHECK(taken(engine).empty());

    engine.play();
    render(engine, 0.5); // one beat: 960 ticks
    REQUIRE(engine.playMidi(0, on(60, 100)));
    render(engine, 0.25);
    REQUIRE(engine.playMidi(0, {0xE0, 0x7F, 0x7F}));
    REQUIRE(engine.playMidi(0, off(60)));
    render(engine, 0.1);
    engine.stop();
    render(engine, 0.05);

    const auto events = taken(engine);
    REQUIRE(events.size() == 3);
    CHECK(events[0].ticks == Approx(960.0));
    CHECK(events[0].channel == 0);
    CHECK(events[2].ticks == Approx(1440.0));

    vloom::MidiTake take;
    for (const auto& event : events)
        take.add(event);
    CHECK(take.held() == 0);
    REQUIRE(take.notes().size() == 1);
    CHECK(take.notes()[0].start == 960);
    CHECK(take.notes()[0].length == 480);
    CHECK(take.notes()[0].key == 60);
    CHECK(take.notes()[0].velocity == Approx(100.0f / 127.0f));
    REQUIRE(take.controls().size() == 1);
    CHECK(take.controls()[0].point.type == vloom::ControlEvent::Type::PitchBend);
    CHECK(take.controls()[0].point.value == 1.0f);
    CHECK(take.controls()[0].point.start == 1440);

    engine.setRecording(false);
    CHECK_FALSE(engine.isRecording());
    engine.play();
    REQUIRE(engine.playMidi(0, on(62)));
    render(engine, 0.05);
    engine.stop();
    CHECK(taken(engine).empty());
}

TEST_CASE("Recording places a note where it was heard: the latency is taken off", "[sequencer][midi]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(synthRack());
    engine.setRecordLatency(2500); // 100 ticks
    engine.setRecording(true);
    engine.play();
    render(engine, 0.5);
    REQUIRE(engine.playMidi(0, on(60)));
    render(engine, 0.05);
    const auto events = taken(engine);
    REQUIRE(events.size() == 1);
    CHECK(events[0].ticks == Approx(860.0));
}

TEST_CASE("Recording in a loop: a note held over the loop's end ends there", "[sequencer][midi]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(synthRack());
    engine.setLoop(0.0, 1920.0);
    engine.setRecordLatency(2500); // 100 ticks
    engine.setRecording(true);
    engine.play();
    render(engine, 45000.0 / kRate); // to 1800 ticks
    REQUIRE(engine.playMidi(0, on(60)));
    render(engine, 4800.0 / kRate); // over the loop's end, to 72 ticks into the second pass
    // Heard 100 ticks back: still in the first pass.
    REQUIRE(engine.playMidi(0, on(64)));
    REQUIRE(engine.playMidi(0, off(60)));
    render(engine, 0.5); // to 1032 ticks
    REQUIRE(engine.playMidi(0, off(64)));
    render(engine, 0.05);
    engine.stop();
    render(engine, 0.05);

    const auto events = taken(engine);
    REQUIRE(events.size() == 4);
    CHECK(events[0].ticks == Approx(1700.0));
    CHECK(events[0].pass == 0);
    CHECK(events[1].ticks == Approx(1892.0));
    CHECK(events[1].pass == 0);
    CHECK(events[3].ticks == Approx(932.0));
    CHECK(events[3].pass == 1);

    vloom::MidiTake take;
    take.setLoop(0.0, 1920.0);
    for (const auto& event : events)
        take.add(event);
    REQUIRE(take.notes().size() == 2);
    CHECK(take.notes()[0].key == 60);
    CHECK(take.notes()[0].start == 1700);
    CHECK(take.notes()[0].length == 192);
    CHECK(take.notes()[1].key == 64);
    CHECK(take.notes()[1].start == 1892);
    CHECK(take.notes()[1].length == 28); // to the loop's end
}

TEST_CASE("A take ends held notes, splits a key played twice, and goes into a pattern", "[sequencer][midi]")
{
    const auto at = [](double ticks, vloom::MidiMessage message, int channel = 0) {
        vloom::RecordedMidi event;
        event.ticks = ticks;
        event.channel = channel;
        event.message = message;
        return event;
    };
    vloom::MidiTake take;
    take.add(at(100.0, on(60)));
    take.add(at(300.0, on(60))); // again without being let go: the first ends here
    take.add(at(400.0, on(62, 64, 5), 1));
    take.add(at(500.0, {0xB0, 64, 127}, 1)); // sustain pedal down
    take.add(at(3900.0, on(67)));
    CHECK(take.held() == 3);
    take.finish(4000.0);
    CHECK(take.held() == 0);
    REQUIRE(take.notes().size() == 4);
    CHECK(take.notes()[0].start == 100);
    CHECK(take.notes()[0].length == 200);

    std::vector<vloom::RecordedNote> notes = take.notes();
    const auto second
        = std::find_if(notes.begin(), notes.end(), [](const auto& n) { return n.channel == 1; });
    REQUIRE(second != notes.end());
    CHECK(second->midiChannel == 5);
    CHECK(second->length == 3600);

    // Into a pattern of one bar (3840 ticks) from the song's start: the
    // note at 3900 wraps to 60; channel 1's note is not channel 0's.
    const auto pattern = vloom::patternNotes(notes, 0, 0, 3840);
    REQUIRE(pattern.size() == 3);
    CHECK(pattern[0].start == 60);
    CHECK(pattern[1].start == 100);
    CHECK(pattern[2].start == 300);
    const auto controls = vloom::patternControls(take.controls(), 1, 0, 3840);
    REQUIRE(controls.size() == 1);
    CHECK(controls[0].number == 64);
    CHECK(controls[0].start == 500);
    CHECK(vloom::patternNotes(notes, 0, 200, 0).size() == 2); // from tick 200 on

    // Quantize to 16ths (240 ticks): all the way, then half of it.
    vloom::quantize(notes, 240, 1.0);
    CHECK(notes[0].start == 0); // 100 is nearer 0 than 240
    CHECK(notes[0].length == 200); // the length stays
    std::vector<vloom::RecordedNote> half = take.notes();
    vloom::quantize(half, 240, 0.5);
    CHECK(half[0].start == 50);
    vloom::quantize(half, 0, 1.0); // no grid: nothing
    CHECK(half[0].start == 50);

    take.clear();
    CHECK(take.notes().empty());
    CHECK(take.controls().empty());
}

TEST_CASE("All notes off, and releaseLiveNotes(), end the notes played live", "[sequencer][midi]")
{
    vloom::Engine engine;
    prepare(engine);
    auto sample = std::make_shared<vloom::SampleBuffer>(1, 96000, kRate);
    for (int i = 0; i < 96000; ++i)
        sample->channel(0)[i] = 0.5f;
    auto state = synthRack();
    vloom::ChannelState sampler;
    sampler.sample = sample;
    sampler.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(sampler);
    engine.setSequencerState(std::move(state));

    // All notes off on MIDI channel 2 ends what is down there, not on channel 0.
    REQUIRE(engine.playMidi(0, on(60, 127, 2)));
    REQUIRE(engine.playMidi(0, on(64, 127, 0)));
    render(engine, 0.1);
    REQUIRE(engine.playMidi(0, {0xB2, 123, 0}));
    REQUIRE(engine.playMidi(0, off(64, 0)));
    REQUIRE(engine.playMidi(0, on(64, 127, 0)));
    render(engine, 0.5);
    REQUIRE(engine.playMidi(0, {0xB0, 120, 0})); // all sound off, on channel 0
    CHECK(loudest(render(engine, 0.5), 20000) == 0.0f);

    // A keyboard gone with keys down on both channels: the engine lets go.
    REQUIRE(engine.playMidi(0, on(67)));
    REQUIRE(engine.playMidi(1, on(60, 127, 9)));
    CHECK(loudest(render(engine, 0.1)) > 0.05f);
    REQUIRE(engine.releaseLiveNotes());
    CHECK(loudest(render(engine, 0.5), 20000) == 0.0f);
}
