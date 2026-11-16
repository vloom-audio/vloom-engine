// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// What the engine routes to and from hosted processors besides their main
// sound: a sidechain, extra outputs, the notes of a note effect, and
// controls. With processors written for the tests, so the routing is
// checked on its own; each plugin format has its own tests for the rest.
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
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
        for (const vloom::ControlEvent& control : context.controls) {
            if (control.type == vloom::ControlEvent::Type::PitchBend)
                bend_ = control.value;
            else if (control.type == vloom::ControlEvent::Type::Controller && control.number == 1)
                wheel_ = control.value;
        }
        std::fill_n(context.channels[0], context.numFrames, bend_);
        std::fill_n(context.channels[1], context.numFrames, wheel_);
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
