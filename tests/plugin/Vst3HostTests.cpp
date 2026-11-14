// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/plugin/Vst3Host.hpp>
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
const std::filesystem::path kTestVst3 = VLOOM_TEST_VST3; // tests/plugin/vst3/TestPlugins.cpp, built

/// The id of the test module's plugin named `name`.
std::string idOf(const vloom::Vst3Library& library, const std::string& name)
{
    for (const vloom::PluginInfo& plugin : library.plugins())
        if (plugin.name == name)
            return plugin.id;
    return {};
}

std::shared_ptr<vloom::PluginInstance> load(const std::string& name)
{
    std::string error;
    auto library = vloom::Vst3Library::open(kTestVst3, &error);
    REQUIRE(library != nullptr);
    auto processor = library->create(idOf(*library, name), &error);
    INFO(error);
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

TEST_CASE("A .vst3 is loaded and lists its audio plugins", "[plugin][vst3]")
{
    std::string error;
    auto library = vloom::Vst3Library::open(kTestVst3, &error);
    INFO(error);
    REQUIRE(library != nullptr);
    REQUIRE(library->plugins().size() == 2); // the controller class is not a plugin
    const auto& level = library->plugins()[0];
    CHECK(level.name == "Level");
    CHECK(level.vendor == "Vloom Tests");
    CHECK(level.version == "1.0.0");
    CHECK(level.kind == vloom::PluginInfo::Kind::Effect);
    CHECK(level.id.size() == 32);
    CHECK(library->plugins()[1].name == "Keys");
    CHECK(library->plugins()[1].kind == vloom::PluginInfo::Kind::Instrument);
    CHECK(vloom::Vst3Library::open(kTestVst3) == library); // loaded once

    CHECK(library->create("no.such.plugin", &error) == nullptr);
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK(vloom::Vst3Library::open(kTestVst3.parent_path() / "missing.vst3", &error) == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("The plugin scan finds .vst3 files", "[plugin][vst3]")
{
    const auto found = vloom::findVst3Files({kTestVst3.parent_path(), kTestVst3.parent_path() / "missing"});
    CHECK(std::find(found.begin(), found.end(), kTestVst3) != found.end());
    CHECK_FALSE(vloom::vst3SearchPaths().empty());
}

TEST_CASE("A VST 3 effect: parameters, processing, state and latency", "[plugin][vst3]")
{
    auto plugin = load("Level");
    const auto parameters = plugin->processor().parameters();
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].id == 3);
    CHECK(parameters[0].name == "Level");
    CHECK(parameters[0].unit == "x");
    CHECK(parameters[0].maxValue == 1.0);
    CHECK(parameters[0].defaultValue == 0.5);
    CHECK(plugin->processor().latencyFrames() == 7);

    std::vector<float> left(64, 0.5f), right(64, -0.5f);
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 0.5f);
    CHECK(right[10] == -0.5f);
    REQUIRE(plugin->setParameter(3, 1.0)); // normalized: x2
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 1.0f);
    CHECK(right[10] == -1.0f);

    const auto state = plugin->processor().saveState();
    REQUIRE_FALSE(state.empty());
    auto other = load("Level");
    REQUIRE(other->processor().loadState(state));
    std::vector<float> ones(16, 1.0f), more(16, 1.0f);
    other->process(ones.data(), more.data(), 16, {});
    CHECK(ones[0] == 2.0f);
    CHECK_FALSE(other->processor().loadState(std::vector<std::uint8_t>{1, 2, 3}));
}

TEST_CASE("A VST 3 effect on an insert", "[plugin][vst3][mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = pluginChannel(load("Keys"), {1});
    state->channels[0].insert = 1;
    engine.setSequencerState(std::move(state));
    auto level = load("Level");
    REQUIRE(level->setParameter(3, 0.0));
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(1);
    mixer->inserts[0].effects = {level};
    engine.setMixerState(std::move(mixer));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(audio[1000 * 2] == 0.0f); // the instrument plays, the effect mutes it
}

TEST_CASE("A VST 3 instrument with its own controller plays a channel's steps on their frames",
    "[plugin][vst3][sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(pluginChannel(load("Keys"), {1}));
    engine.play();
    const auto audio = render(engine, 0.25);
    const float on = audio[0];
    CHECK(on == Approx(0.1f)); // the step's note from frame 0
    CHECK(audio[5999 * 2] == on); // held for the step
    CHECK(audio[6000 * 2] == 0.0f); // ended on the step's last tick
    CHECK(audio[1] == Approx(0.12f)); // told it plays, at 120 BPM
}

TEST_CASE("A VST 3 instrument plays piano-roll notes, overlapping", "[plugin][vst3][sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    std::vector<vloom::Note> notes{{0, 240, 60, 1.0f}, {120, 240, 64, 1.0f}};
    engine.setSequencerState(pluginChannel(load("Keys"), {}, notes));
    engine.play();
    const auto audio = render(engine, 0.25);
    CHECK(audio[2999 * 2] == Approx(0.1f));
    CHECK(audio[3000 * 2] == Approx(0.2f)); // the second note starts
    CHECK(audio[6000 * 2] == Approx(0.1f)); // the first ends
    CHECK(audio[9000 * 2] == 0.0f); // and the second
}

TEST_CASE("Bypassing a VST 3 instrument ends its held notes", "[plugin][vst3]")
{
    auto keys = load("Keys");
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
    CHECK(l[10] == 0.0f); // not hanging: the host ended it
}

TEST_CASE("Hosted VST 3 plugins do not allocate on the audio thread", "[plugin][vst3]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = pluginChannel(load("Keys"), {1, 0, 1, 1}, {{0, 60, 60, 1.0f}, {30, 400, 67, 0.5f}});
    state->channels[0].insert = 1;
    engine.setSequencerState(std::move(state));
    auto level = load("Level");
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
            level->setParameter(3, 0.001 * block); // control side: outside the count
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
