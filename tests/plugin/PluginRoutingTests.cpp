// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// What the engine routes to and from hosted processors besides their main
// sound: a sidechain, extra outputs, the notes of a note effect, and
// controls. With processors written for the tests, so the routing is
// checked on its own; each plugin format has its own tests for the rest.
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/mixer/MixerGraph.hpp>
#include <vloom/engine/mixer/MixerState.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// An instrument for the tests. Left: 0.1 per key held. Right: the highest
/// key held / 1000. Its extra output k (0, 1, ...) carries 0.2 + 0.1 * k
/// while any key is held. With `drone`, it sounds as if one key were always
/// held.
class Keys final : public vloom::AudioProcessor {
public:
    explicit Keys(int extraOutputs = 0, bool drone = false)
        : extraOutputs_(extraOutputs)
        , drone_(drone)
    {
    }
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.keys", "Keys", "Tests", "1", vloom::PluginInfo::Kind::Instrument};
    }
    bool activate(double, int) override { return true; }
    [[nodiscard]] int extraOutputCount() const override { return extraOutputs_; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        std::size_t next = 0;
        for (int i = 0; i < context.numFrames; ++i) {
            for (; next < context.notes.size() && context.notes[next].frame <= i; ++next)
                held_[static_cast<std::size_t>(context.notes[next].key)]
                    = context.notes[next].type == vloom::NoteEvent::Type::On;
            int count = drone_ ? 1 : 0;
            int highest = 0;
            for (int key = 0; key < 128; ++key)
                if (held_[static_cast<std::size_t>(key)]) {
                    ++count;
                    highest = key;
                }
            context.channels[0][i] += 0.1f * static_cast<float>(count);
            context.channels[1][i] += static_cast<float>(highest) / 1000.0f;
            for (std::size_t k = 0; k + 1 < context.extraOutputs.size(); k += 2) {
                const bool mine = k / 2 < static_cast<std::size_t>(extraOutputs_); // silence past its last
                const float level = mine && count > 0 ? 0.2f + 0.1f * static_cast<float>(k / 2) : 0.0f;
                context.extraOutputs[k][i] = level;
                context.extraOutputs[k + 1][i] = level;
            }
        }
    }
    void reset() noexcept override { held_.fill(false); }

private:
    int extraOutputs_;
    bool drone_;
    std::array<bool, 128> held_{};
};

/// An effect that turns its input down by what its sidechain carries:
/// out = in * (1 - |sidechain|). Without a sidechain it passes its input.
class Ducker final : public vloom::AudioProcessor {
public:
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.ducker", "Ducker", "Tests", "1", {}};
    }
    bool activate(double, int) override { return true; }
    [[nodiscard]] bool hasSidechain() const override { return true; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        if (context.sidechain == nullptr)
            return;
        for (int ch = 0; ch < context.numChannels && ch < 2; ++ch)
            for (int i = 0; i < context.numFrames; ++i)
                context.channels[ch][i] *= 1.0f - std::abs(context.sidechain[ch][i]);
    }
};

/// A note effect: every note it gets, it plays an octave higher as well.
class Octaver final : public vloom::AudioProcessor {
public:
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.octaver", "Octaver", "Tests", "1", {}};
    }
    bool activate(double, int) override { return true; }
    [[nodiscard]] bool makesNotes() const override { return true; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        if (context.notesOutCount == nullptr)
            return;
        std::size_t made = 0;
        for (const vloom::NoteEvent& note : context.notes)
            for (const int up : {0, 12}) {
                if (made == context.notesOut.size() || note.key + up > 127)
                    continue;
                vloom::NoteEvent out = note;
                out.key = static_cast<std::int16_t>(note.key + up);
                context.notesOut[made++] = out;
            }
        *context.notesOutCount = static_cast<int>(made);
    }
};

/// A note effect that starts notes and never ends them, and one that makes
/// its notes in the wrong order: what a host has to put right.
class Latch final : public vloom::AudioProcessor {
public:
    explicit Latch(bool backwards = false)
        : backwards_(backwards)
    {
    }
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.latch", "Latch", "Tests", "1", {}};
    }
    bool activate(double, int) override { return true; }
    [[nodiscard]] bool makesNotes() const override { return true; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        if (context.notesOutCount == nullptr)
            return;
        std::size_t made = 0;
        for (const vloom::NoteEvent& note : context.notes)
            if (note.type == vloom::NoteEvent::Type::On && made < context.notesOut.size())
                context.notesOut[made++] = note;
        if (backwards_)
            std::reverse(
                context.notesOut.begin(), context.notesOut.begin() + static_cast<std::ptrdiff_t>(made));
        *context.notesOutCount = static_cast<int>(made);
    }
    void reset() noexcept override { ++resets; }
    int resets = 0;

private:
    bool backwards_;
};

/// An instrument that tells notes on different channels apart, as an MPE
/// one does. Left: 0.1 per note held, a note being a key on a channel.
/// Right: the pitch bends of the channels that have a note held, added up.
class Poly final : public vloom::AudioProcessor {
public:
    [[nodiscard]] vloom::PluginInfo info() const override { return {"test.poly", "Poly", "Tests", "1", {}}; }
    bool activate(double, int) override { return true; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        for (const vloom::ControlEvent& control : context.controls)
            if (control.type == vloom::ControlEvent::Type::PitchBend)
                bend_[static_cast<std::size_t>(control.channel)] = control.value;
        for (const vloom::NoteEvent& note : context.notes)
            held_[static_cast<std::size_t>(note.channel)][static_cast<std::size_t>(note.key)]
                = note.type == vloom::NoteEvent::Type::On;
        int notes = 0;
        float bends = 0.0f;
        for (std::size_t channel = 0; channel < 16; ++channel) {
            const auto down = std::count(held_[channel].begin(), held_[channel].end(), true);
            notes += static_cast<int>(down);
            if (down > 0)
                bends += bend_[channel];
        }
        std::fill_n(context.channels[0], context.numFrames, 0.1f * static_cast<float>(notes));
        std::fill_n(context.channels[1], context.numFrames, bends);
    }

private:
    std::array<std::array<bool, 128>, 16> held_{};
    std::array<float, 16> bend_{};
};

/// An effect with latency: it puts its input out 100 frames late and says so.
class Late final : public vloom::AudioProcessor {
public:
    static constexpr int kFrames = 100;
    [[nodiscard]] vloom::PluginInfo info() const override { return {"test.late", "Late", "Tests", "1", {}}; }
    bool activate(double, int) override { return true; }
    [[nodiscard]] int latencyFrames() const override { return kFrames; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        for (int ch = 0; ch < context.numChannels && ch < 2; ++ch)
            for (int i = 0; i < context.numFrames; ++i) {
                float& kept = line_[static_cast<std::size_t>(ch)][at_[static_cast<std::size_t>(ch)]];
                std::swap(kept, context.channels[ch][i]);
                at_[static_cast<std::size_t>(ch)] = (at_[static_cast<std::size_t>(ch)] + 1) % kFrames;
            }
    }

private:
    std::array<std::array<float, kFrames>, 2> line_{};
    std::array<std::size_t, 2> at_{};
};

/// An instrument that shows the controls it got: left = the last pitch
/// bend, right = the last value of controller 1.
class Wheels final : public vloom::AudioProcessor {
public:
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.wheels", "Wheels", "Tests", "1", {}};
    }
    bool activate(double, int) override { return true; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        std::size_t next = 0;
        for (int i = 0; i < context.numFrames; ++i) {
            for (; next < context.controls.size() && context.controls[next].frame <= i; ++next) {
                const vloom::ControlEvent& control = context.controls[next];
                if (control.type == vloom::ControlEvent::Type::PitchBend)
                    bend_ = control.value;
                else if (control.type == vloom::ControlEvent::Type::Controller && control.number == 1)
                    wheel_ = control.value;
            }
            context.channels[0][i] = bend_;
            context.channels[1][i] = wheel_;
        }
    }

private:
    float bend_ = 0.0f;
    float wheel_ = 0.0f;
};

template <typename Processor, typename... Arguments>
std::shared_ptr<vloom::PluginInstance> make(Arguments&&... arguments)
{
    auto plugin = std::make_shared<vloom::PluginInstance>(
        std::make_unique<Processor>(std::forward<Arguments>(arguments)...));
    REQUIRE(plugin->activate(kRate, vloom::kMaxBlockFrames));
    return plugin;
}

vloom::ChannelState channel(std::shared_ptr<vloom::PluginInstance> plugin, int insert, bool step = false)
{
    vloom::ChannelState c;
    c.instrument = vloom::Instrument::Plugin;
    c.plugin = std::move(plugin);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    c.insert = insert;
    if (step)
        c.steps = {1};
    return c;
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
    engine.setTempo(120.0); // a 16th step: 6000 frames
    engine.setMetronomeEnabled(false);
}

std::vector<float> render(vloom::Engine& engine, double seconds)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = 256;
    return vloom::renderInterleaved(engine, options);
}

} // namespace

TEST_CASE("A send can feed an insert's sidechain instead of its sound", "[plugin][mixer][routing]")
{
    // Two droning channels at 0.1: one on insert 1 with a ducker, one on
    // insert 2, which only sends to insert 1, at gain 4.
    const auto master = [](bool sidechain) {
        vloom::Engine engine;
        prepare(engine);
        auto state = std::make_unique<vloom::SequencerState>();
        state->channels.push_back(channel(make<Keys>(0, true), 1));
        state->channels.push_back(channel(make<Keys>(0, true), 2));
        engine.setSequencerState(std::move(state));
        auto mixer = std::make_unique<vloom::MixerState>();
        mixer->inserts.resize(2);
        mixer->inserts[0].effects = {make<Ducker>()};
        mixer->inserts[1].output = -1; // nowhere: only its send is heard, if at all
        vloom::Send send;
        send.target = 1;
        send.gain = 4.0f;
        send.sidechain = sidechain;
        mixer->inserts[1].sends = {send};
        engine.setMixerState(std::move(mixer));
        engine.play();
        return render(engine, 0.1)[2000 * 2];
    };
    // Into the sidechain: insert 1's own 0.1, turned down by 0.4. Not added to it.
    CHECK(master(true) == Approx(0.1f * (1.0f - 0.4f)));
    // An ordinary send joins the sound instead: 0.1 + 0.4, and the ducker hears no sidechain.
    CHECK(master(false) == Approx(0.5f));
}

TEST_CASE("A sidechain nobody listens to does not pile up", "[plugin][mixer][routing]")
{
    // Insert 1 has no effects and no sound of its own: what is sent to its
    // sidechain is dropped every block, and nothing of it is ever heard.
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(0, true), 2));
    engine.setSequencerState(std::move(state));
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(2);
    mixer->inserts[1].output = -1;
    vloom::Send send;
    send.target = 1;
    send.sidechain = true;
    mixer->inserts[1].sends = {send};
    engine.setMixerState(std::move(mixer));
    engine.play();
    const auto audio = render(engine, 0.2);
    CHECK(*std::max_element(audio.begin(), audio.end()) == 0.0f);
}

TEST_CASE(
    "An instrument's extra outputs play into the inserts chosen for them", "[plugin][sequencer][routing]")
{
    const auto master = [](std::vector<int> outputs) {
        vloom::Engine engine;
        prepare(engine);
        auto state = std::make_unique<vloom::SequencerState>();
        state->channels.push_back(channel(make<Keys>(2), 0, true));
        state->channels[0].pluginOutputs = std::move(outputs);
        engine.setSequencerState(std::move(state));
        auto mixer = std::make_unique<vloom::MixerState>();
        mixer->inserts.resize(2);
        mixer->inserts[0].gain = 0.5f; // insert 1 at half
        mixer->inserts[1].output = -1; // insert 2 nowhere
        engine.setMixerState(std::move(mixer));
        engine.play();
        return render(engine, 0.1)[1000 * 2];
    };
    CHECK(master({}) == Approx(0.1f)); // the main output only
    CHECK(master({1}) == Approx(0.1f + 0.5f * 0.2f)); // output 2 through insert 1
    CHECK(master({0, 1}) == Approx(0.1f + 0.2f + 0.5f * 0.3f)); // output 2 to the master, 3 through insert 1
    CHECK(master({2, -1}) == Approx(0.1f)); // into an insert that goes nowhere; not played
    CHECK(master({1, 1, 1, 1}) == Approx(0.1f + 0.5f * (0.2f + 0.3f))); // more than it has: silence
}

TEST_CASE("A note effect's notes are what the instrument plays", "[plugin][sequencer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(), 0, true));
    state->channels[0].noteEffect = make<Octaver>();
    const int root = state->channels[0].rootKey;
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.25);
    CHECK(audio[1000 * 2] == Approx(0.2f)); // the step's key and the one an octave up
    CHECK(audio[1000 * 2 + 1] == Approx(static_cast<float>(root + 12) / 1000.0f));
    CHECK(audio[6400 * 2] == 0.0f); // both ended with the step
}

TEST_CASE("Controls sent to an instance reach the processor with its next block", "[plugin][routing]")
{
    auto wheels = make<Wheels>();
    using Type = vloom::ControlEvent::Type;
    REQUIRE(wheels->sendControl(Type::PitchBend, 0, -0.5f));
    REQUIRE(wheels->sendControl(Type::Controller, 1, 0.25f));
    REQUIRE(wheels->sendControl(Type::Controller, 7, 1.0f)); // another controller: not shown
    CHECK_FALSE(wheels->sendControl(Type::Controller, 1, std::numeric_limits<float>::quiet_NaN()));
    std::vector<float> left(64), right(64);
    wheels->process(left.data(), right.data(), 64, {});
    CHECK(left[0] == -0.5f);
    CHECK(right[0] == 0.25f);
    REQUIRE(wheels->sendControl(Type::PitchBend, 0, 9.0f)); // out of range: clamped
    wheels->process(left.data(), right.data(), 64, {});
    CHECK(left[0] == 1.0f);
}

TEST_CASE(
    "Sidechains, extra outputs and note effects do not allocate on the audio thread", "[plugin][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(2), 1, true));
    state->channels[0].steps = {1, 0, 1, 1};
    state->channels[0].pluginOutputs = {2, 0};
    state->channels[0].noteEffect = make<Octaver>();
    engine.setSequencerState(std::move(state));
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(2);
    mixer->inserts[0].effects = {make<Ducker>()};
    vloom::Send send;
    send.target = 1;
    send.sidechain = true;
    mixer->inserts[1].sends = {send};
    engine.setMixerState(std::move(mixer));
    engine.play();
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 400; ++block) {
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}

TEST_CASE("A note effect that is off or makes no notes lets the channel's notes through",
    "[plugin][sequencer][routing]")
{
    const auto left = [](std::shared_ptr<vloom::PluginInstance> effect, bool bypass) {
        vloom::Engine engine;
        prepare(engine);
        auto state = std::make_unique<vloom::SequencerState>();
        state->channels.push_back(channel(make<Keys>(), 0, true));
        effect->setBypassed(bypass);
        state->channels[0].noteEffect = std::move(effect);
        engine.setSequencerState(std::move(state));
        engine.play();
        return render(engine, 0.1)[1000 * 2];
    };
    CHECK(left(make<Octaver>(), false) == Approx(0.2f)); // at work: two keys
    CHECK(left(make<Octaver>(), true) == Approx(0.1f)); // bypassed: the channel's own key
    CHECK(left(make<Ducker>(), false) == Approx(0.1f)); // no note effect at all: the same
}

TEST_CASE("Switching a note effect off ends the notes it made", "[plugin][sequencer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    auto effect = make<Octaver>();
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(), 0));
    state->channels[0].notes = {{0, 960, 60, 1.0f}}; // one long note: 24000 frames
    state->channels[0].noteEffect = effect;
    engine.setSequencerState(std::move(state));
    engine.play();
    CHECK(render(engine, 0.1)[2000 * 2] == Approx(0.2f)); // both keys held
    effect->setBypassed(true); // their note-offs would now never come from the effect
    const auto after = render(engine, 0.1);
    CHECK(after[2000 * 2] == 0.0f); // the host ended them: nothing hangs
}

TEST_CASE("Stopping the song ends what a note effect left sounding", "[plugin][sequencer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    auto latch = std::make_unique<Latch>();
    Latch* seen = latch.get();
    auto effect = std::make_shared<vloom::PluginInstance>(std::move(latch));
    REQUIRE(effect->activate(kRate, vloom::kMaxBlockFrames));
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(), 0, true));
    state->channels[0].noteEffect = effect;
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto playing = render(engine, 0.2);
    CHECK(playing[9000 * 2] == Approx(0.1f)); // past the step's end: the latch never let go
    engine.stop();
    const auto stopped = render(engine, 0.1);
    CHECK(stopped[4000 * 2] == 0.0f); // the host ended the key
    CHECK(seen->resets > 0); // and told the effect
}

TEST_CASE("Notes a processor makes out of order reach the host in frame order", "[plugin][routing]")
{
    auto latch = make<Latch>(true);
    std::vector<float> left(64), right(64);
    const std::array<vloom::NoteEvent, 3> notes{vloom::NoteEvent{2, vloom::NoteEvent::Type::On, 60, 1.0f},
        vloom::NoteEvent{9, vloom::NoteEvent::Type::On, 62, 1.0f},
        vloom::NoteEvent{30, vloom::NoteEvent::Type::On, 64, 1.0f}};
    std::array<vloom::NoteEvent, 8> made{};
    int count = 0;
    vloom::PluginInstance::Io io;
    io.notesOut = made;
    io.notesOutCount = &count;
    latch->process(left.data(), right.data(), 64, {}, notes, io);
    REQUIRE(count == 3);
    CHECK(made[0].frame == 2);
    CHECK(made[1].frame == 9);
    CHECK(made[2].frame == 30);
    CHECK(made[2].key == 64);
}

TEST_CASE("Note effects in a row each get what the one before made", "[plugin][sequencer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(), 0, true));
    state->channels[0].noteEffect = make<Octaver>();
    state->channels[0].noteEffects = {make<Octaver>()};
    const int root = state->channels[0].rootKey;
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.25);
    // The key, an octave up, and two octaves up (the middle one twice: one key).
    CHECK(audio[1000 * 2] == Approx(0.3f));
    CHECK(audio[1000 * 2 + 1] == Approx(static_cast<float>(root + 24) / 1000.0f));
    CHECK(audio[6400 * 2] == 0.0f);
}

TEST_CASE(
    "A note played by hand on the instrument goes through its note effect", "[plugin][sequencer][routing]")
{
    const auto left = [](bool withEffect) {
        vloom::Engine engine;
        prepare(engine);
        auto instrument = make<Keys>();
        auto state = std::make_unique<vloom::SequencerState>();
        state->channels.push_back(channel(instrument, 0));
        if (withEffect)
            state->channels[0].noteEffect = make<Octaver>();
        engine.setSequencerState(std::move(state));
        REQUIRE(instrument->playNote(60, 1.0f));
        return render(engine, 0.05)[1000 * 2];
    };
    CHECK(left(false) == Approx(0.1f));
    CHECK(left(true) == Approx(0.2f)); // the key and its octave
}

TEST_CASE(
    "An effect behind one with latency hears the sidechain as late as its sound", "[plugin][mixer][routing]")
{
    // Insert 1: a drone at 0.1 through an effect 100 frames late, then a
    // ducker. Insert 2 sends a note that ends at frame 6000 to its sidechain
    // at gain 4. The ducker works on sound that is 100 frames late, so the
    // duck has to end 100 frames late as well: at 6100.
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(0, true), 1));
    state->channels.push_back(channel(make<Keys>(), 2, true));
    engine.setSequencerState(std::move(state));
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(2);
    mixer->inserts[0].effects = {make<Late>(), make<Ducker>()};
    mixer->inserts[1].output = -1;
    vloom::Send send;
    send.target = 1;
    send.gain = 4.0f;
    send.sidechain = true;
    mixer->inserts[1].sends = {send};
    engine.setMixerState(std::move(mixer));
    engine.play();
    const auto audio = render(engine, 0.2);
    CHECK(audio[3000 * 2] == Approx(0.1f * 0.6f)); // ducked while the note sounds
    CHECK(audio[6050 * 2] == Approx(0.1f * 0.6f)); // still: the sound here is from before the note's end
    CHECK(audio[6150 * 2] == Approx(0.1f)); // and over
}

TEST_CASE("The routes a mixer leaves out can be asked for", "[plugin][mixer][routing]")
{
    vloom::MixerState mixer;
    mixer.inserts.resize(3);
    mixer.inserts[0].output = 2; // 1 -> 2
    mixer.inserts[1].output = 1; // 2 -> 1: closes a loop
    mixer.inserts[2].output = -1; // nowhere, on purpose
    vloom::Send toItself;
    toItself.target = 3;
    vloom::Send nowhere;
    nowhere.target = 9000; // no such insert
    nowhere.sidechain = true;
    mixer.inserts[2].sends = {toItself, nowhere};
    const auto dropped = vloom::droppedRoutes(mixer);
    REQUIRE(dropped.size() == 3);
    CHECK(dropped[0].source == 2);
    CHECK(dropped[0].slot == 0);
    CHECK(dropped[0].target == 1);
    CHECK(dropped[1].source == 3);
    CHECK(dropped[1].slot == 1);
    CHECK(dropped[2].slot == 2);
    CHECK(dropped[2].sidechain);
    CHECK(vloom::droppedRoutes(vloom::MixerState{}).empty());
}

TEST_CASE("Controls written into a pattern reach the plugin on their frames", "[plugin][sequencer][routing]")
{
    using Type = vloom::ControlEvent::Type;
    vloom::Engine engine;
    prepare(engine); // a tick is 25 frames
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Wheels>(), 0));
    // Not in order, as an app may give them.
    state->channels[0].controls = {{240, Type::PitchBend, 0, -1.0f}, {0, Type::PitchBend, 0, 0.5f},
        {120, Type::Controller, 1, 0.75f}, {120, Type::Controller, 7, 0.1f}, {-5, Type::PitchBend, 0, 0.9f}};
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.2);
    CHECK(audio[0] == 0.5f); // the bend at tick 0, from the first frame
    CHECK(audio[2999 * 2 + 1] == 0.0f); // the wheel: not before its tick
    CHECK(audio[3000 * 2 + 1] == 0.75f); // on its frame
    CHECK(audio[5999 * 2] == 0.5f);
    CHECK(audio[6000 * 2] == -1.0f); // the second bend, on its frame

    // Stopped with the pitch bent: it goes back to the centre.
    engine.stop();
    const auto stopped = render(engine, 0.05);
    CHECK(stopped[2000 * 2] == 0.0f);
    CHECK(stopped[2000 * 2 + 1] == 0.75f); // a controller keeps its value
}

TEST_CASE("Controls of a pattern play where its clip sits in the song", "[plugin][sequencer][routing]")
{
    using Type = vloom::ControlEvent::Type;
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Wheels>(), 0));
    state->song = true;
    vloom::SongPattern pattern;
    pattern.controls = {{{0, Type::PitchBend, 0, 0.25f}, {120, Type::PitchBend, 0, 0.5f}}};
    state->patterns.push_back(pattern);
    vloom::SongPatternClip clip;
    clip.pattern = 0;
    clip.startTick = 240; // frame 6000
    clip.lengthTicks = 480;
    state->patternClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.3);
    CHECK(audio[5999 * 2] == 0.0f); // before the clip
    CHECK(audio[6000 * 2] == 0.25f); // the clip's start: the pattern's tick 0
    CHECK(audio[9000 * 2] == 0.5f); // 120 ticks into it
}

TEST_CASE("The song's controls keep their frames in a block longer than the processor's", "[plugin][routing]")
{
    using Type = vloom::ControlEvent::Type;
    auto wheels = std::make_shared<vloom::PluginInstance>(std::make_unique<Wheels>());
    REQUIRE(wheels->activate(kRate, 64)); // blocks of 64: a block of 200 is processed in four parts
    std::vector<float> left(200), right(200);
    const std::array<vloom::ControlEvent, 2> controls{vloom::ControlEvent{70, Type::PitchBend, 0, 0.5f},
        vloom::ControlEvent{150, Type::PitchBend, 0, 1.0f}};
    vloom::PluginInstance::Io io;
    io.controls = controls;
    wheels->process(left.data(), right.data(), 200, {}, {}, io);
    CHECK(left[69] == 0.0f);
    CHECK(left[70] == 0.5f);
    CHECK(left[149] == 0.5f);
    CHECK(left[150] == 1.0f);
}

TEST_CASE("A channel's instrument plays another channel's notes too", "[plugin][sequencer][routing]")
{
    // One channel has a step and an octaver, and is not heard (its insert
    // goes nowhere). The other has no notes of its own and plays the first
    // one's. Whichever comes first in the rack, from the first frame.
    const auto heard = [](bool sourceFirst) {
        vloom::Engine engine;
        prepare(engine);
        auto state = std::make_unique<vloom::SequencerState>();
        vloom::ChannelState source = channel(make<Keys>(), 1, true);
        source.noteEffect = make<Octaver>();
        vloom::ChannelState target = channel(make<Keys>(), 0);
        target.notesFrom = sourceFirst ? 0 : 1;
        if (sourceFirst) {
            state->channels.push_back(std::move(source));
            state->channels.push_back(std::move(target));
        } else {
            state->channels.push_back(std::move(target));
            state->channels.push_back(std::move(source));
        }
        engine.setSequencerState(std::move(state));
        auto mixer = std::make_unique<vloom::MixerState>();
        mixer->inserts.resize(1);
        mixer->inserts[0].output = -1;
        engine.setMixerState(std::move(mixer));
        engine.play();
        const auto audio = render(engine, 0.3);
        CHECK(audio[6400 * 2] == 0.0f); // and the notes end with the source's
        return audio[0];
    };
    CHECK(heard(true) == Approx(0.2f)); // the step's key and its octave, as the source's instrument got them
    CHECK(heard(false) == Approx(0.2f));
}

TEST_CASE(
    "Channels that name each other as the source of their notes still play", "[plugin][sequencer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(make<Keys>(), 0, true));
    state->channels.push_back(channel(make<Keys>(), 0, true));
    state->channels[0].notesFrom = 1;
    state->channels[1].notesFrom = 0;
    state->channels[1].rootKey = 72; // another key than the first one's
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.3);
    // The circle is cut at one place: one channel plays both keys, the other its own.
    CHECK(audio[1000 * 2] == Approx(0.3f));
    CHECK(audio[6400 * 2] == 0.0f); // and nothing hangs
}

TEST_CASE("Taking another channel's notes away ends the ones held", "[plugin][sequencer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    const auto sourcePlugin = make<Keys>();
    const auto targetPlugin = make<Keys>();
    const auto sourceVoices = std::make_shared<vloom::ChannelVoices>();
    const auto targetVoices = std::make_shared<vloom::ChannelVoices>();
    const auto rack = [&](int from) {
        auto state = std::make_unique<vloom::SequencerState>();
        vloom::ChannelState source = channel(sourcePlugin, 1);
        source.voices = sourceVoices;
        source.notes = {{0, 960, 60, 1.0f}}; // one long note: 24000 frames
        vloom::ChannelState target = channel(targetPlugin, 0);
        target.voices = targetVoices;
        target.notesFrom = from;
        state->channels.push_back(std::move(source));
        state->channels.push_back(std::move(target));
        return state;
    };
    engine.setSequencerState(rack(0));
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(1);
    mixer->inserts[0].output = -1; // the source is not heard
    engine.setMixerState(std::move(mixer));
    engine.play();
    CHECK(render(engine, 0.1)[2000 * 2] == Approx(0.1f)); // the source's note, on the other channel
    engine.setSequencerState(rack(-1)); // no longer: its note-off would never come
    CHECK(render(engine, 0.1)[2000 * 2] == 0.0f);
}

TEST_CASE("Note-effect chains, shared notes, the song's controls and a delayed sidechain do not allocate",
    "[plugin][routing]")
{
    using Type = vloom::ControlEvent::Type;
    vloom::Engine engine;
    prepare(engine);
    auto state = std::make_unique<vloom::SequencerState>();
    // A channel with two note effects in a row and controls written into its pattern ...
    state->channels.push_back(channel(make<Keys>(2), 1, true));
    state->channels[0].steps = {1, 0, 1, 1};
    state->channels[0].notes = {{30, 400, 67, 0.5f}};
    state->channels[0].pluginOutputs = {2, 0};
    state->channels[0].noteEffect = make<Octaver>();
    state->channels[0].noteEffects = {make<Octaver>()};
    state->channels[0].controls = {{0, Type::PitchBend, 0, 0.5f}, {100, Type::Controller, 1, 0.3f},
        {300, Type::PitchBend, 0, -0.5f}, {700, Type::Pressure, 0, 0.8f}};
    // ... one that plays its notes too, with controls of its own ...
    state->channels.push_back(channel(make<Wheels>(), 2));
    state->channels[1].notesFrom = 0;
    state->channels[1].controls = {{50, Type::PitchBend, 0, 1.0f}};
    // ... and one that names the second as its source.
    state->channels.push_back(channel(make<Keys>(), 0, true));
    state->channels[2].notesFrom = 1;
    engine.setSequencerState(std::move(state));
    // Insert 2 sends to the sidechain of insert 1, whose ducker sits behind an effect with latency.
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(2);
    mixer->inserts[0].effects = {make<Late>(), make<Ducker>()};
    vloom::Send send;
    send.target = 1;
    send.sidechain = true;
    mixer->inserts[1].sends = {send};
    engine.setMixerState(std::move(mixer));
    engine.play();
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    float heard = 0.0f;
    for (int block = 0; block < 600; ++block) {
        if (block == 300)
            engine.stop(); // ends the notes the effects made, centres the bend
        if (block == 320)
            engine.play();
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
        for (float sample : l)
            heard = std::max(heard, std::abs(sample));
    }
    CHECK(onAudioThread == 0);
    CHECK(heard > 0.0f); // and it did play
}

TEST_CASE(
    "Notes and controls keep their MIDI channel, so each note bends on its own", "[plugin][routing][mpe]")
{
    using Type = vloom::ControlEvent::Type;
    auto poly = make<Poly>();
    std::vector<float> left(64), right(64);
    // The same key twice, on two channels: two notes for an MPE instrument.
    REQUIRE(poly->playNote(60, 1.0f, 1));
    REQUIRE(poly->playNote(60, 1.0f, 2));
    REQUIRE(poly->sendControl(Type::PitchBend, 0, 0.5f, 2));
    REQUIRE(poly->sendControl(Type::PitchBend, 0, 0.25f, 7)); // a channel with no note on it
    poly->process(left.data(), right.data(), 64, {});
    CHECK(left[0] == Approx(0.2f));
    CHECK(right[0] == Approx(0.5f)); // channel 2's bend, and no other
    REQUIRE(poly->stopNote(60, 2)); // ends the one on channel 2 only
    poly->process(left.data(), right.data(), 64, {});
    CHECK(left[0] == Approx(0.1f));
    CHECK(right[0] == 0.0f);
    // Out of range: put on the nearest channel, not past the instrument's tables.
    REQUIRE(poly->playNote(61, 1.0f, 99));
    REQUIRE(poly->stopNote(60, 1));
    poly->process(left.data(), right.data(), 64, {});
    CHECK(left[0] == Approx(0.1f));
}

TEST_CASE("A control written into the song goes out on its channel", "[plugin][sequencer][routing][mpe]")
{
    using Type = vloom::ControlEvent::Type;
    vloom::Engine engine;
    prepare(engine);
    auto poly = make<Poly>();
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels.push_back(channel(poly, 0));
    state->channels[0].controls = {{0, Type::PitchBend, 0, 0.5f, 3}, {0, Type::PitchBend, 0, 1.0f, 4}};
    engine.setSequencerState(std::move(state));
    REQUIRE(poly->playNote(64, 1.0f, 3));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(audio[1000 * 2] == Approx(0.1f));
    CHECK(audio[1000 * 2 + 1] == Approx(0.5f)); // channel 3's bend reached the note on channel 3
}
