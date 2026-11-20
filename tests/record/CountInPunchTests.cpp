// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/record/AudioTake.hpp>
#include <vloom/engine/sample/WavReader.hpp>
#include <vloom/engine/sequencer/MidiTake.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

// 120 BPM at 48 kHz: 0.04 ticks a frame (kPpq = 960), 24000 frames a beat.
constexpr double kRate = 48000.0;
constexpr double kTicksPerFrame = 0.04;

struct TempFolder {
    std::filesystem::path path;
    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("vloom_punch_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    [[nodiscard]] std::string file(const char* name) const { return (path / name).string(); }
};

/// What the input holds at `frame`: its index, scaled into what a WAV file
/// reads back unclipped (exact in a float: a power of two).
float indexed(std::int64_t frame) { return static_cast<float>(frame) / 65536.0f; }

/// An engine with nothing to play, fed an input whose every frame holds its
/// own index (indexed()), so a recording shows which frames it kept.
/// Everything it plays is kept in `output` (left channel).
struct Rig {
    vloom::Engine engine;
    int blockSize;
    std::vector<float> left;
    std::vector<float> right;
    std::array<float*, 2> out{};
    std::vector<float> in;
    std::int64_t frame = 0; ///< frames rendered so far
    std::vector<float> output;
    std::vector<float> recorded; ///< interleaved, as the engine handed it over
    std::vector<vloom::AudioRecordMark> marks;

    explicit Rig(int block = 256, double rate = kRate)
        : blockSize(block)
        , left(static_cast<std::size_t>(block))
        , right(static_cast<std::size_t>(block))
        , in(static_cast<std::size_t>(block))
    {
        out = {left.data(), right.data()};
        engine.prepare(rate, block);
        engine.setTempo(120.0);
        engine.setMetronomeEnabled(false);
    }

    void block()
    {
        for (int i = 0; i < blockSize; ++i)
            in[static_cast<std::size_t>(i)] = indexed(frame + i);
        engine.pushAudioInput(in.data(), nullptr, blockSize);
        engine.process(out.data(), 2, blockSize);
        output.insert(output.end(), left.begin(), left.end());
        frame += blockSize;
    }
    /// Renders blocks until at least `frames` were rendered in all.
    void renderTo(std::int64_t frames)
    {
        while (frame < frames)
            block();
    }
    /// What was recorded so far, as the control thread takes it.
    void take()
    {
        std::vector<float> chunk(2 * 4096);
        for (std::size_t got = 0; (got = engine.takeRecordedAudio(chunk)) > 0;)
            recorded.insert(
                recorded.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(2 * got));
        std::array<vloom::AudioRecordMark, 64> got{};
        for (std::size_t n = 0; (n = engine.takeAudioMarks(got)) > 0;)
            marks.insert(marks.end(), got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n));
    }
    [[nodiscard]] std::size_t recordedFrames() const { return recorded.size() / 2; }
    /// The input frame recorded at `i`.
    [[nodiscard]] float recordedAt(std::size_t i) const { return recorded[2 * i] * 65536.0f; }
};

/// The frame an event at `ticks` fires on, by the engine's rule: the first
/// sample at or after its exact time.
std::int64_t frameOf(double beats, double bpm, double rate)
{
    return static_cast<std::int64_t>(std::ceil(beats * 60.0 * rate / bpm - 1e-6));
}

vloom::MidiMessage on(int key, int velocity = 127)
{
    return {0x90, static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity)};
}
vloom::MidiMessage off(int key) { return {0x80, static_cast<std::uint8_t>(key), 0}; }

std::unique_ptr<vloom::SequencerState> synthRack()
{
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState synth;
    synth.instrument = vloom::Instrument::Synth;
    synth.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(synth);
    return state;
}

std::vector<vloom::RecordedMidi> takenMidi(vloom::Engine& engine)
{
    std::array<vloom::RecordedMidi, 64> events{};
    const std::size_t count = engine.takeRecordedMidi(events);
    return {events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count)};
}

/// Renders until `ticks` (at 120 BPM, 48 kHz); with blocks of 100 frames
/// (4 ticks), MIDI played then is heard on that very tick.
void playTo(Rig& rig, double ticks)
{
    rig.renderTo(static_cast<std::int64_t>(std::llround(ticks / kTicksPerFrame)));
}

} // namespace

TEST_CASE(
    "A count-in lasts its bars at the tempo and time signature, whatever the block size", "[engine][countin]")
{
    struct Case {
        double bpm;
        int beatsPerBar;
        int bars;
        double rate;
    };
    const std::array cases{
        Case{120.0, 4, 1, 48000.0}, // 96000 frames
        Case{140.0, 3, 2, 44100.0}, // 113400
        Case{133.0, 7, 1, 48000.0}, // 151578.9...: the song starts on the next sample
        Case{90.0, 6, 1, 96000.0}, // 384000
    };
    for (const Case& c : cases) {
        for (const int block : {37, 256, 1000, 4096}) {
            INFO("bpm " << c.bpm << ", " << c.beatsPerBar << " beats, " << c.bars << " bars, " << c.rate
                        << " Hz, blocks of " << block);
            Rig rig(block, c.rate);
            rig.engine.setTempo(c.bpm);
            rig.engine.setBeatsPerBar(c.beatsPerBar);
            rig.engine.setCountIn(c.bars);
            rig.engine.setAudioRecording(true);
            rig.engine.locate(1920.0); // the song stays here while it counts
            rig.engine.play();
            rig.block();
            CHECK(rig.engine.isCountingIn());
            CHECK_FALSE(rig.engine.isPlaying());
            CHECK(rig.engine.countInBarsLeft() == c.bars);
            CHECK(rig.engine.positionTicks() == 1920.0);
            CHECK(rig.engine.beatsPerBar() == c.beatsPerBar);

            const int beats = c.bars * c.beatsPerBar;
            const std::int64_t length = frameOf(beats, c.bpm, c.rate);
            rig.renderTo(length + 1000);
            CHECK_FALSE(rig.engine.isCountingIn());
            CHECK(rig.engine.countInBarsLeft() == 0);
            CHECK(rig.engine.isPlaying());
            rig.take();
            // The first frame recorded is the one the song started on.
            REQUIRE(rig.recordedFrames() > 0);
            CHECK(static_cast<std::int64_t>(rig.recordedAt(0)) == length);
            REQUIRE_FALSE(rig.marks.empty());
            CHECK(rig.marks[0].kind == vloom::AudioRecordMark::Kind::Start);
            CHECK(rig.marks[0].ticks == 1920.0);
            CHECK(rig.marks[0].shiftFrames == 0);

            // It clicked on every beat (the metronome is off: the count-in clicks anyway).
            for (int k = 0; k < beats; ++k) {
                const auto at = static_cast<std::size_t>(frameOf(k, c.bpm, c.rate));
                CHECK(rig.output[at] != 0.0f);
                if (k > 0)
                    CHECK(rig.output[at - 1] == 0.0f);
            }
            // The song itself is silent here: nothing after the count-in.
            for (auto i = static_cast<std::size_t>(length); i < rig.output.size(); ++i)
                if (rig.output[i] != 0.0f) {
                    FAIL("sound after the count-in at frame " << i);
                    break;
                }
        }
    }
}

TEST_CASE("The count-in's bars and beats left can be read while it counts", "[engine][countin]")
{
    Rig rig;
    rig.engine.setCountIn(2);
    rig.engine.setRecording(true); // MIDI armed is enough
    rig.engine.play();
    rig.renderTo(36000); // 1.5 beats
    CHECK(rig.engine.countInBeatsLeft() == 7);
    CHECK(rig.engine.countInBarsLeft() == 2);
    rig.renderTo(108000); // 4.5 beats
    CHECK(rig.engine.countInBeatsLeft() == 4);
    CHECK(rig.engine.countInBarsLeft() == 1);
    rig.renderTo(8 * 24000 + 256);
    CHECK(rig.engine.countInBeatsLeft() == 0);
    CHECK(rig.engine.isPlaying());
}

TEST_CASE("A count-in only comes with recording armed; stop cancels it", "[engine][countin]")
{
    SECTION("nothing armed: the song starts at once")
    {
        Rig rig;
        rig.engine.setCountIn(1);
        rig.engine.play();
        rig.block();
        CHECK_FALSE(rig.engine.isCountingIn());
        CHECK(rig.engine.isPlaying());
    }
    SECTION("stop during the count-in: the song never starts, and stays where it was")
    {
        Rig rig;
        rig.engine.setCountIn(1);
        rig.engine.setAudioRecording(true);
        rig.engine.locate(960.0);
        rig.engine.play();
        rig.renderTo(48000);
        CHECK(rig.engine.isCountingIn());
        rig.engine.stop();
        rig.renderTo(200000);
        CHECK_FALSE(rig.engine.isCountingIn());
        CHECK_FALSE(rig.engine.isPlaying());
        CHECK(rig.engine.positionTicks() == 960.0);
        rig.take();
        CHECK(rig.recordedFrames() == 0);
    }
    SECTION("the count-in can be silent when the metronome is off")
    {
        Rig rig;
        rig.engine.setCountIn(1);
        rig.engine.setCountInClick(false);
        rig.engine.setAudioRecording(true);
        rig.engine.play();
        rig.renderTo(96000 + 512);
        CHECK(rig.engine.isPlaying());
        for (const float s : rig.output)
            REQUIRE(s == 0.0f);
    }
}

TEST_CASE(
    "After a count-in the take starts exactly at its position, the round trip left out", "[engine][countin]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setSequencerState(synthRack());
    rig.engine.setCountIn(1);
    rig.engine.setAudioRecordLatency(512);
    rig.engine.setRecordLatency(2500); // MIDI: 100 ticks
    rig.engine.setAudioRecording(true);
    rig.engine.setRecording(true);
    rig.engine.locate(3840.0); // bar 2
    rig.engine.play();
    rig.renderTo(48000);
    REQUIRE(rig.engine.playMidi(0, on(60))); // during the count-in: played, not recorded
    REQUIRE(rig.engine.playMidi(0, off(60)));
    rig.renderTo(96000 + 512); // the song started at frame 96000
    REQUIRE(rig.engine.playMidi(0, on(62))); // heard 100 ticks back: in the count-in
    rig.renderTo(96000 + 4096);
    REQUIRE(rig.engine.playMidi(0, off(62)));
    rig.renderTo(120000);
    rig.engine.stop();
    rig.block();

    vloom::AudioTake take;
    REQUIRE(take.open(folder.file("countin.wav"), 48000));
    REQUIRE(take.finish(rig.engine));
    const auto segments = take.segments();
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].newTake);
    CHECK(segments[0].firstFrame == 0);
    CHECK(segments[0].startTicks == Approx(3840.0).margin(1e-9));
    // The input at the song's first frame comes back 512 frames later: that is the file's first frame.
    const auto file = vloom::readWavFile(folder.file("countin.wav"));
    REQUIRE(file.buffer.has_value());
    CHECK(file.buffer->channel(0)[0] == indexed(96000 + 512));

    const auto events = takenMidi(rig.engine);
    REQUIRE(events.size() == 2);
    CHECK(events[0].ticks == Approx(3840.0)); // placed on the song's first beat
    CHECK(events[0].message.data1 == 62);
    CHECK(events[1].ticks > 3840.0);
}

TEST_CASE("Punch recording keeps only the range, to the sample, mid-block too", "[engine][punch]")
{
    for (const int latency : {0, 1000}) {
        INFO("round trip " << latency);
        TempFolder folder;
        Rig rig;
        rig.engine.setAudioRecordLatency(latency);
        rig.engine.setPunch(100.0, 300.0); // frames 2500 .. 7500: not on a block boundary
        rig.engine.setAudioRecording(true);
        rig.engine.setInputMonitor(0, 1.0f); // still heard outside the range
        rig.engine.play();
        rig.renderTo(40 * 256);
        rig.engine.stop();
        rig.block();
        CHECK(rig.output[300] == indexed(300)); // monitored before the punch-in

        vloom::AudioTake take;
        REQUIRE(take.open(folder.file("punch.wav"), 48000));
        REQUIRE(take.finish(rig.engine));
        CHECK(take.frames() == 5000);
        const auto segments = take.segments();
        REQUIRE(segments.size() == 1);
        CHECK(segments[0].newTake);
        CHECK(segments[0].firstFrame == 0);
        CHECK(segments[0].frames == 5000);
        CHECK(segments[0].startTicks == Approx(100.0).margin(1e-9));
        const auto file = vloom::readWavFile(folder.file("punch.wav"));
        REQUIRE(file.buffer.has_value());
        REQUIRE(file.buffer->frames() == 5000);
        // Heard at the punch-in: rendered there, back `latency` frames later.
        CHECK(file.buffer->channel(0)[0] == indexed(2500 + latency));
        CHECK(file.buffer->channel(0)[4999] == indexed(7499 + latency));
    }
}

TEST_CASE("Punch recording in a loop keeps the range in every pass", "[engine][punch]")
{
    SECTION("a range inside the loop")
    {
        Rig rig;
        rig.engine.setLoop(0.0, 480.0); // 12000 frames
        rig.engine.setPunch(100.0, 300.0);
        rig.engine.setAudioRecordLatency(1000);
        rig.engine.setAudioRecording(true);
        rig.engine.play();
        rig.renderTo(150 * 256); // 38400 frames: three passes and a bit
        rig.engine.stop();
        rig.block();

        TempFolder folder;
        vloom::AudioTake take;
        REQUIRE(take.open(folder.file("loop.wav"), 48000));
        REQUIRE(take.finish(rig.engine));
        const auto segments = take.segments();
        REQUIRE(segments.size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(segments[i].pass == static_cast<std::int32_t>(i));
            CHECK(segments[i].firstFrame == static_cast<std::int64_t>(5000 * i));
            CHECK(segments[i].frames == 5000);
            CHECK(segments[i].startTicks == Approx(100.0).margin(1e-9));
            CHECK(segments[i].newTake == (i == 0));
        }
        const auto file = vloom::readWavFile(folder.file("loop.wav"));
        REQUIRE(file.buffer.has_value());
        CHECK(file.buffer->channel(0)[0] == indexed(3500));
        CHECK(file.buffer->channel(0)[5000] == indexed(15500));
        CHECK(file.buffer->channel(0)[10000] == indexed(27500));
    }
    SECTION("a range up to the loop's end: the latency carries it over the jump back")
    {
        Rig rig;
        rig.engine.setLoop(0.0, 480.0);
        rig.engine.setPunch(200.0, 480.0); // frames 5000 .. 12000 as heard
        rig.engine.setAudioRecordLatency(1000);
        rig.engine.setAudioRecording(true);
        rig.engine.play();
        rig.renderTo(26000);
        rig.engine.stop();
        rig.block();
        rig.take();

        // Pass 0 is heard from 6000 to 13000 as rendered: 1000 frames into pass 1.
        REQUIRE(rig.recordedFrames() == 14000);
        CHECK(rig.recordedAt(0) == 6000.0f);
        CHECK(rig.recordedAt(6999) == 12999.0f);
        CHECK(rig.recordedAt(7000) == 18000.0f);
        CHECK(rig.recordedAt(13999) == 24999.0f);

        const auto& marks = rig.marks;
        REQUIRE(marks.size() == 3); // the take's start, and a stretch per pass
        CHECK(marks[1].kind == vloom::AudioRecordMark::Kind::Punch);
        CHECK(marks[1].frame == 0);
        CHECK(marks[1].ticks == Approx(200.0).margin(1e-9));
        CHECK(marks[1].pass == 0);
        CHECK(marks[2].kind == vloom::AudioRecordMark::Kind::Punch);
        CHECK(marks[2].frame == 7000);
        CHECK(marks[2].ticks == Approx(200.0).margin(1e-9));
        CHECK(marks[2].pass == 1);
    }
}

TEST_CASE("Punch recording of MIDI keeps what was heard inside the range", "[engine][punch][midi]")
{
    Rig rig(100);
    rig.engine.setSequencerState(synthRack());
    rig.engine.setPunch(960.0, 1920.0);
    rig.engine.setRecording(true);
    rig.engine.play();
    playTo(rig, 480.0);
    REQUIRE(rig.engine.playMidi(0, on(60))); // before the punch-in: not recorded
    REQUIRE(rig.engine.playMidi(0, {0xB0, 1, 64})); // nor this
    playTo(rig, 1200.0);
    REQUIRE(rig.engine.playMidi(0, off(60))); // a note-off: passed on, ignored by the take
    REQUIRE(rig.engine.playMidi(0, on(62)));
    playTo(rig, 1300.0);
    REQUIRE(rig.engine.playMidi(0, {0xB0, 1, 100}));
    playTo(rig, 1500.0);
    REQUIRE(rig.engine.playMidi(0, off(62)));
    playTo(rig, 1800.0);
    REQUIRE(rig.engine.playMidi(0, on(64))); // held over the punch-out
    playTo(rig, 2000.0);
    REQUIRE(rig.engine.playMidi(0, {0xB0, 1, 30})); // after it: not recorded
    REQUIRE(rig.engine.playMidi(0, on(65)));
    playTo(rig, 2400.0);
    REQUIRE(rig.engine.playMidi(0, off(64)));
    REQUIRE(rig.engine.playMidi(0, off(65)));
    playTo(rig, 2500.0);
    rig.engine.stop();
    rig.block();

    const auto events = takenMidi(rig.engine);
    // off 60, on 62, controller, off 62, on 64, off 64, off 65
    REQUIRE(events.size() == 7);
    vloom::MidiTake take;
    take.setPunch(960.0, 1920.0);
    for (const auto& event : events)
        take.add(event);
    take.finish(2500.0);
    REQUIRE(take.notes().size() == 2);
    CHECK(take.notes()[0].key == 62);
    CHECK(take.notes()[0].start == 1200);
    CHECK(take.notes()[0].length == 300);
    CHECK(take.notes()[1].key == 64);
    CHECK(take.notes()[1].start == 1800);
    CHECK(take.notes()[1].length == 120); // ended at the punch-out
    REQUIRE(take.controls().size() == 1);
    CHECK(take.controls()[0].point.start == 1300);
}

TEST_CASE(
    "Punch recording of MIDI in a loop: each pass, notes ended at the punch-out", "[engine][punch][midi]")
{
    Rig rig(100);
    rig.engine.setSequencerState(synthRack());
    rig.engine.setLoop(0.0, 1920.0);
    rig.engine.setPunch(480.0, 1440.0);
    rig.engine.setRecording(true);
    rig.engine.play();
    playTo(rig, 960.0);
    REQUIRE(rig.engine.playMidi(0, on(60))); // held over the punch-out and the loop's end
    playTo(rig, 1920.0 + 240.0);
    REQUIRE(rig.engine.playMidi(0, off(60)));
    playTo(rig, 1920.0 + 600.0);
    REQUIRE(rig.engine.playMidi(0, on(62)));
    playTo(rig, 1920.0 + 700.0);
    REQUIRE(rig.engine.playMidi(0, off(62)));
    playTo(rig, 1920.0 + 800.0);
    rig.engine.stop();
    rig.block();

    vloom::MidiTake take;
    take.setLoop(0.0, 1920.0);
    take.setPunch(480.0, 1440.0);
    for (const auto& event : takenMidi(rig.engine))
        take.add(event);
    REQUIRE(take.notes().size() == 2);
    CHECK(take.notes()[0].key == 60);
    CHECK(take.notes()[0].start == 960);
    CHECK(take.notes()[0].length == 480);
    CHECK(take.notes()[1].key == 62);
    CHECK(take.notes()[1].start == 600);
    CHECK(take.notes()[1].length == 100);
}

TEST_CASE("A take with a punch range leaves out notes begun outside it", "[sequencer][punch][midi]")
{
    const auto at = [](double ticks, vloom::MidiMessage message, std::int32_t pass = 0) {
        vloom::RecordedMidi event;
        event.ticks = ticks;
        event.pass = pass;
        event.message = message;
        return event;
    };
    vloom::MidiTake take;
    take.setPunch(1000.0, 2000.0);
    take.add(at(900.0, on(60))); // down before the punch-in, held into it: left out
    take.add(at(1100.0, off(60)));
    take.add(at(1500.0, on(61))); // still held when the take finishes
    take.add(at(1999.0, on(62)));
    take.add(at(2000.0, on(63))); // on the punch-out: outside
    take.add(at(2100.0, off(63)));
    take.finish(2500.0);
    REQUIRE(take.notes().size() == 2);
    CHECK(take.notes()[0].key == 61);
    CHECK(take.notes()[0].length == 500); // ended at the punch-out, not the finish
    CHECK(take.notes()[1].key == 62);
    CHECK(take.notes()[1].length == 1);

    take.clear();
    take.setPunch(0.0, 0.0); // off: everything again
    take.add(at(900.0, on(60)));
    take.add(at(1100.0, off(60)));
    REQUIRE(take.notes().size() == 1);
    CHECK(take.notes()[0].length == 200);
}

TEST_CASE("Count-in and punch recording do not allocate on the audio thread", "[engine][countin][punch]")
{
    Rig rig;
    rig.engine.setSequencerState(synthRack());
    rig.engine.setInputMonitor(0, 0.5f);
    rig.engine.setLoop(0.0, 960.0);
    rig.engine.setPunch(200.0, 700.0);
    rig.engine.setCountIn(1);
    rig.engine.setAudioRecordLatency(700);
    rig.engine.setMetronomeEnabled(true);
    rig.engine.setAudioRecording(true);
    rig.engine.setRecording(true);
    rig.engine.play();
    std::vector<float> drain(2 * 4096);
    std::array<vloom::AudioRecordMark, 16> marks{};
    std::array<vloom::RecordedMidi, 16> midi{};
    long long allocations = 0;
    for (int b = 0; b < 1200; ++b) { // the count-in, then about four passes
        if (b % 40 == 0)
            rig.engine.playMidi(0, b % 80 == 0 ? on(60) : off(60));
        for (int i = 0; i < rig.blockSize; ++i)
            rig.in[static_cast<std::size_t>(i)] = 0.1f;
        const vloom::test::AllocationCounter counter;
        rig.engine.pushAudioInput(rig.in.data(), nullptr, rig.blockSize);
        rig.engine.process(rig.out.data(), 2, rig.blockSize);
        allocations += counter.count();
        rig.engine.takeRecordedAudio(drain);
        rig.engine.takeAudioMarks(marks);
        rig.engine.takeRecordedMidi(midi);
    }
    CHECK(allocations == 0);
    CHECK(rig.engine.isPlaying());
}
