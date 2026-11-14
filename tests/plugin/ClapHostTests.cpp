// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/plugin/ClapHost.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
const std::filesystem::path kTestClap = VLOOM_TEST_CLAP; // tests/plugin/clap/TestPlugins.cpp, built

std::shared_ptr<vloom::PluginInstance> load(const std::string& id)
{
    std::string error;
    auto library = vloom::ClapLibrary::open(kTestClap, &error);
    REQUIRE(library != nullptr);
    auto processor = library->create(id, &error);
    REQUIRE(processor != nullptr);
    auto plugin = std::make_shared<vloom::PluginInstance>(std::move(processor));
    REQUIRE(plugin->activate(kRate, vloom::kMaxBlockFrames));
    return plugin;
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
    engine.setTempo(120.0); // a 16th step: 6000 frames
    engine.setMetronomeEnabled(false);
}

/// One channel played by `plugin`, with `steps` and `notes`.
std::unique_ptr<vloom::SequencerState> pluginChannel(std::shared_ptr<vloom::PluginInstance> plugin,
    std::vector<std::uint8_t> steps, std::vector<vloom::Note> notes = {})
{
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.instrument = vloom::Instrument::Plugin;
    c.plugin = std::move(plugin);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    c.steps = std::move(steps);
    c.notes = std::move(notes);
    state->channels.push_back(std::move(c));
    return state;
}

std::vector<float> render(vloom::Engine& engine, double seconds)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = 256;
    return vloom::renderInterleaved(engine, options);
}

} // namespace

TEST_CASE("A .clap is loaded and lists its plugins", "[plugin][clap]")
{
    std::string error;
    auto library = vloom::ClapLibrary::open(kTestClap, &error);
    REQUIRE(library != nullptr);
    REQUIRE(library->plugins().size() == 2);
    CHECK(library->plugins()[0].id == "vloom.test.level");
    CHECK(library->plugins()[0].kind == vloom::PluginInfo::Kind::Effect);
    CHECK(library->plugins()[1].id == "vloom.test.keys");
    CHECK(library->plugins()[1].kind == vloom::PluginInfo::Kind::Instrument);
    CHECK(vloom::ClapLibrary::open(kTestClap) == library); // loaded once

    CHECK(library->create("no.such.plugin", &error) == nullptr);
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK(vloom::ClapLibrary::open(kTestClap.parent_path() / "missing.clap", &error) == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("The plugin scan finds .clap files", "[plugin][clap]")
{
    const auto found = vloom::findClapFiles({kTestClap.parent_path(), kTestClap.parent_path() / "missing"});
    CHECK(std::find(found.begin(), found.end(), kTestClap) != found.end());
    CHECK_FALSE(vloom::clapSearchPaths().empty());
}

TEST_CASE("A CLAP effect: parameters, processing, state and latency", "[plugin][clap]")
{
    auto plugin = load("vloom.test.level");
    const auto parameters = plugin->processor().parameters();
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].id == 3);
    CHECK(parameters[0].name == "Level");
    CHECK(parameters[0].maxValue == 2.0);
    CHECK(plugin->processor().latencyFrames() == 7);

    std::vector<float> left(64, 0.5f), right(64, -0.5f);
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 0.5f);
    CHECK(right[10] == -0.5f);
    REQUIRE(plugin->setParameter(3, 2.0));
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 1.0f);
    CHECK(right[10] == -1.0f);

    const auto state = plugin->processor().saveState();
    REQUIRE(state.size() == sizeof(double));
    auto other = load("vloom.test.level");
    REQUIRE(other->processor().loadState(state));
    std::vector<float> ones(16, 1.0f), more(16, 1.0f);
    other->process(ones.data(), more.data(), 16, {});
    CHECK(ones[0] == 2.0f);
}

TEST_CASE("A CLAP effect on an insert", "[plugin][clap][mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    auto keys = load("vloom.test.keys");
    auto state = pluginChannel(keys, {1});
    state->channels[0].insert = 1;
    engine.setSequencerState(std::move(state));
    auto level = load("vloom.test.level");
    REQUIRE(level->setParameter(3, 0.0));
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(1);
    mixer->inserts[0].effects = {level};
    engine.setMixerState(std::move(mixer));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(audio[1000 * 2] == 0.0f); // the instrument plays, the effect mutes it
}

TEST_CASE("A CLAP instrument plays a channel's steps on their frames", "[plugin][clap][sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(pluginChannel(load("vloom.test.keys"), {1}));
    engine.play();
    const auto audio = render(engine, 0.25);
    const float on = audio[0];
    CHECK(on > 0.05f); // the step's note from frame 0
    CHECK(audio[5999 * 2] == on); // held for the step
    CHECK(audio[6000 * 2] == 0.0f); // ended on the step's last tick
    CHECK(audio[1] > 0.0f); // the plugin is told it plays, and the tempo
}

TEST_CASE("A CLAP instrument plays piano-roll notes, overlapping", "[plugin][clap][sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    // 240 ticks = one step = 6000 frames.
    std::vector<vloom::Note> notes{{0, 240, 60, 1.0f}, {120, 240, 64, 1.0f}};
    engine.setSequencerState(pluginChannel(load("vloom.test.keys"), {}, notes));
    engine.play();
    const auto audio = render(engine, 0.25);
    const float one = audio[100 * 2];
    REQUIRE(one > 0.05f);
    CHECK(audio[2999 * 2] == Approx(one));
    CHECK(audio[3000 * 2] == Approx(2 * one)); // the second note starts
    CHECK(audio[6000 * 2] == Approx(one)); // the first ends
    CHECK(audio[9000 * 2] == 0.0f); // and the second
}

TEST_CASE("Stopping ends a CLAP instrument's held notes", "[plugin][clap][sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    std::vector<vloom::Note> notes{{0, 100 * 240, 60, 1.0f}}; // a long note
    engine.setSequencerState(pluginChannel(load("vloom.test.keys"), {}, notes));
    engine.play();
    CHECK(render(engine, 0.01)[200] > 0.0f);
    engine.stop();
    const auto audio = render(engine, 0.01);
    CHECK(audio[300 * 2] == 0.0f);
}

TEST_CASE("prepare() activates a channel's plugin before the audio thread picked it up", "[plugin][clap]")
{
    vloom::Engine engine;
    prepare(engine);
    auto keys = load("vloom.test.keys");
    keys->deactivate();
    engine.setSequencerState(pluginChannel(keys, {1}));
    engine.prepare(kRate, 256); // no block in between
    CHECK(keys->isActive());
}

TEST_CASE("Bypassing a CLAP instrument ends its held notes", "[plugin][clap]")
{
    auto keys = load("vloom.test.keys");
    std::vector<float> l(64), r(64);
    const std::array<vloom::NoteEvent, 1> on{vloom::NoteEvent{0, vloom::NoteEvent::Type::On, 60, 1.0f}};
    keys->process(l.data(), r.data(), 64, {}, on);
    REQUIRE(l[10] > 0.05f);
    keys->setBypassed(true);
    const std::array<vloom::NoteEvent, 1> off{vloom::NoteEvent{0, vloom::NoteEvent::Type::Off, 60, 0.0f}};
    keys->process(l.data(), r.data(), 64, {}, off); // the off is dropped while bypassed
    keys->setBypassed(false);
    std::fill(l.begin(), l.end(), 0.0f);
    keys->process(l.data(), r.data(), 64, {});
    CHECK(l[10] == 0.0f); // not hanging
}

TEST_CASE("Hosted CLAP plugins do not allocate on the audio thread", "[plugin][clap]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state
        = pluginChannel(load("vloom.test.keys"), {1, 0, 1, 1}, {{0, 60, 60, 1.0f}, {30, 400, 67, 0.5f}});
    state->channels[0].insert = 1;
    engine.setSequencerState(std::move(state));
    auto level = load("vloom.test.level");
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(1);
    mixer->inserts[0].effects = {level};
    engine.setMixerState(std::move(mixer));
    engine.play();
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 400; ++block) {
        if (block % 10 == 0)
            level->setParameter(3, 0.01 * block); // control side: outside the count
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
