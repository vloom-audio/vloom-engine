// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/EditorRecorder.hpp"
#include "plugin/PluginFiles.hpp"
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/plugin/Vst2Host.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
// tests/plugin/vst2/TestPlugins.cpp, built twice.
const std::filesystem::path kTestLevel = VLOOM_TEST_VST2_LEVEL;
const std::filesystem::path kTestKeys = VLOOM_TEST_VST2_KEYS;

std::shared_ptr<vloom::PluginInstance> load(const std::filesystem::path& file)
{
    std::string error;
    auto library = vloom::Vst2Library::open(file, &error);
    INFO(error);
    REQUIRE(library != nullptr);
    REQUIRE(library->plugins().size() == 1);
    auto processor = library->create(library->plugins()[0].id, &error);
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

TEST_CASE("A VST 2 file is loaded and tells what its plugin is", "[plugin][vst2]")
{
    std::string error;
    auto library = vloom::Vst2Library::open(kTestLevel, &error);
    INFO(error);
    REQUIRE(library != nullptr);
    REQUIRE(library->plugins().size() == 1);
    const auto& level = library->plugins()[0];
    CHECK(level.name == "Test Level");
    CHECK(level.vendor == "Vloom Tests");
    CHECK(level.version == "1000");
    CHECK(level.id == "564C5431"); // 'VLT1'
    CHECK(level.kind == vloom::PluginInfo::Kind::Effect);
    CHECK(vloom::Vst2Library::open(kTestLevel) == library); // loaded once

    auto keys = vloom::Vst2Library::open(kTestKeys, &error);
    REQUIRE(keys != nullptr);
    CHECK(keys->plugins()[0].name == "Test Keys");
    CHECK(keys->plugins()[0].kind == vloom::PluginInfo::Kind::Instrument);

    CHECK(library->create("no.such.plugin", &error) == nullptr);
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK(vloom::Vst2Library::open(kTestLevel.parent_path() / "missing.dll", &error) == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("A library that is not a VST 2 plugin is refused", "[plugin][vst2]")
{
    // A real shared library with no VST 2 entry point: the test .clap.
    std::string error;
    CHECK(vloom::Vst2Library::open(VLOOM_TEST_CLAP, &error) == nullptr);
    CHECK(error.find("not a VST 2 plugin") != std::string::npos);
}

TEST_CASE("The plugin scan finds VST 2 files", "[plugin][vst2]")
{
    const auto found = vloom::findVst2Files({kTestLevel.parent_path(), kTestLevel.parent_path() / "missing"});
    CHECK(std::find(found.begin(), found.end(), kTestLevel) != found.end());
    CHECK(std::find(found.begin(), found.end(), kTestKeys) != found.end());
    CHECK_FALSE(vloom::vst2SearchPaths().empty());
}

TEST_CASE("A VST 2 effect: parameters, processing, state and latency", "[plugin][vst2]")
{
    auto plugin = load(kTestLevel);
    const auto parameters = plugin->processor().parameters();
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].id == 0);
    CHECK(parameters[0].name == "Level");
    CHECK(parameters[0].unit == "x");
    CHECK(parameters[0].maxValue == 1.0);
    CHECK(parameters[0].defaultValue == 0.5);
    CHECK(plugin->processor().latencyFrames() == 7);
    CHECK(plugin->processor().parameterValue(0) == 0.5);
    CHECK_FALSE(plugin->processor().parameterValue(1).has_value());
    CHECK_FALSE(plugin->processor().hasFailed());
    CHECK_FALSE(plugin->activate(kRate, 0)); // no block size: refused, not thrown
    REQUIRE(plugin->activate(kRate, vloom::kMaxBlockFrames));

    std::vector<float> left(64, 0.5f), right(64, -0.5f);
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 0.5f);
    CHECK(right[10] == -0.5f);
    REQUIRE(plugin->setParameter(0, 1.0)); // x2
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 1.0f);
    CHECK(right[10] == -1.0f);
    CHECK(plugin->processor().parameterValue(0) == 1.0);

    const auto state = plugin->processor().saveState();
    REQUIRE(state.size() == 5 + sizeof(float)); // "VLV2", 'P', one value
    auto other = load(kTestLevel);
    REQUIRE(other->processor().loadState(state));
    std::vector<float> ones(16, 1.0f), more(16, 1.0f);
    other->process(ones.data(), more.data(), 16, {});
    CHECK(ones[0] == 2.0f);
    CHECK_FALSE(other->processor().loadState(std::vector<std::uint8_t>{1, 2, 3}));
    // A chunk for a plugin that keeps its state as parameters.
    CHECK_FALSE(other->processor().loadState(std::vector<std::uint8_t>{'V', 'L', 'V', '2', 'C', 1, 2}));
}

TEST_CASE("A VST 2 instrument keeps its state as a chunk", "[plugin][vst2]")
{
    auto keys = load(kTestKeys);
    const auto state = keys->processor().saveState();
    REQUIRE(state.size() == 9);
    CHECK(state[4] == 'C');
    CHECK(state[5] == 't');
    CHECK(load(kTestKeys)->processor().loadState(state));
}

TEST_CASE("A VST 2 instrument plays a channel's steps on their blocks", "[plugin][vst2][sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(pluginChannel(load(kTestKeys), {1}));
    engine.play();
    const auto audio = render(engine, 0.25);
    CHECK(audio[0] == Approx(0.1f)); // the step's note
    CHECK(audio[5000 * 2] == Approx(0.1f)); // held for the step
    CHECK(audio[6400 * 2] == 0.0f); // and ended (the test plugin counts per block of 256)
    CHECK(audio[1] == Approx(0.04f)); // told the song is in 4/4
}

TEST_CASE("Bypassing a VST 2 instrument ends its held notes", "[plugin][vst2]")
{
    auto keys = load(kTestKeys);
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

TEST_CASE("Hosted VST 2 plugins do not allocate on the audio thread", "[plugin][vst2]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = pluginChannel(load(kTestKeys), {1, 0, 1, 1}, {{0, 60, 60, 1.0f}, {30, 400, 67, 0.5f}});
    state->channels[0].insert = 1;
    engine.setSequencerState(std::move(state));
    auto level = load(kTestLevel);
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
            level->setParameter(0, 0.001 * block); // control side: outside the count
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}

#if !defined(__linux__) // no VST 2 editors on Linux
TEST_CASE("A VST 2 editor opens in a window, asks for a size and reports its edits", "[plugin][vst2][editor]")
{
    auto plugin = load(kTestLevel);
    vloom::AudioProcessor& level = plugin->processor();
    vloom::test::EditorRecorder recorder;

    REQUIRE(level.hasEditor());
    CHECK(level.editorSize().width == 0); // closed
    CHECK_FALSE(level.openEditor(nullptr, &recorder));
    REQUIRE(level.openEditor(vloom::test::fakeWindow(), &recorder));
    CHECK(level.isEditorOpen());

    // It asked for 400 x 300 as it opened, and took it.
    REQUIRE(recorder.resizes.size() == 1);
    CHECK(recorder.resizes[0].width == 400);
    CHECK(level.editorSize().width == 400);
    CHECK(level.editorSize().height == 300);
    CHECK_FALSE(level.canResizeEditor());

    // Its edit (parameter 0 to 0.25: gain 0.5) reached the host, and the plugin has it.
    REQUIRE(recorder.edits.size() == 1);
    CHECK(recorder.edits[0].id == 0);
    CHECK(recorder.edits[0].value == Approx(0.25));
    std::vector<float> left(64, 1.0f);
    std::vector<float> right(64, 1.0f);
    plugin->process(left.data(), right.data(), 64, vloom::TransportInfo{});
    CHECK(left[0] == Approx(0.5f));

    level.idle();
    level.closeEditor();
    CHECK_FALSE(level.isEditorOpen());
    CHECK(level.editorSize().width == 0);
}
#endif

TEST_CASE("A VST 2 plugin without an editor says so", "[plugin][vst2][editor]")
{
    auto keys = load(kTestKeys);
    vloom::test::EditorRecorder recorder;
    CHECK_FALSE(keys->processor().hasEditor());
    CHECK_FALSE(keys->processor().openEditor(vloom::test::fakeWindow(), &recorder));
}

#if defined(VLOOM_TEST_BRIDGE) // Windows: the same plugins, in a bridge process (ADR 0011)

namespace {

std::shared_ptr<vloom::PluginInstance> loadBridged(const std::filesystem::path& file)
{
    std::string error;
    auto library = vloom::Vst2Library::openBridged(file, VLOOM_TEST_BRIDGE, &error);
    INFO(error);
    REQUIRE(library != nullptr);
    REQUIRE(library->isBridged());
    REQUIRE(library->plugins().size() == 1);
    auto processor = library->create(library->plugins()[0].id, &error);
    INFO(error);
    REQUIRE(processor != nullptr);
    auto plugin = std::make_shared<vloom::PluginInstance>(std::move(processor));
    REQUIRE(plugin->activate(kRate, vloom::kMaxBlockFrames));
    return plugin;
}

} // namespace

TEST_CASE("A file open in the host can be opened in a bridge too", "[plugin][vst2][bridge]")
{
    // Asking for a bridge must never give the library whose plugin runs in
    // the host, however the file was opened before.
    auto inProcess = vloom::Vst2Library::open(kTestLevel);
    REQUIRE(inProcess != nullptr);
    CHECK_FALSE(inProcess->isBridged());
    auto bridged = vloom::Vst2Library::openBridged(kTestLevel, VLOOM_TEST_BRIDGE);
    REQUIRE(bridged != nullptr);
    CHECK(bridged->isBridged());
    CHECK(bridged != inProcess);
    CHECK(vloom::Vst2Library::openBridged(kTestLevel, VLOOM_TEST_BRIDGE) == bridged);
    CHECK(vloom::Vst2Library::open(kTestLevel) == inProcess);
}

TEST_CASE("A bridge that cannot be started is an error, not a crash", "[plugin][vst2][bridge]")
{
    std::string error;
    CHECK(vloom::Vst2Library::openBridged(kTestLevel, kTestLevel.parent_path() / "no-bridge.exe", &error)
        == nullptr);
    CHECK_FALSE(error.empty());
    error.clear();
    // A real bridge and a library that is no VST 2 plugin: the bridge says why.
    CHECK(vloom::Vst2Library::openBridged(VLOOM_TEST_CLAP, VLOOM_TEST_BRIDGE, &error) == nullptr);
    CHECK(error.find("not a VST 2 plugin") != std::string::npos);
}

TEST_CASE("A VST 2 effect in the bridge: parameters, processing, state and latency", "[plugin][vst2][bridge]")
{
    auto plugin = loadBridged(kTestLevel);
    CHECK(plugin->processor().info().name == "Test Level");
    CHECK(plugin->processor().info().vendor == "Vloom Tests");
    CHECK(plugin->processor().info().id == "564C5431");
    const auto parameters = plugin->processor().parameters();
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0].name == "Level");
    CHECK(parameters[0].unit == "x");
    CHECK(parameters[0].defaultValue == 0.5);
    CHECK(plugin->processor().latencyFrames() == 7);

    std::vector<float> left(64, 0.5f), right(64, -0.5f);
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 0.5f);
    CHECK(right[10] == -0.5f);
    REQUIRE(plugin->setParameter(0, 1.0)); // x2
    plugin->process(left.data(), right.data(), 64, {});
    CHECK(left[10] == 1.0f);
    CHECK(right[10] == -1.0f);

    CHECK(plugin->processor().parameterValue(0) == 1.0);
    CHECK_FALSE(plugin->processor().parameterValue(7).has_value());

    // The state reads the same whether the plugin is bridged or not.
    const auto state = plugin->processor().saveState();
    REQUIRE(state.size() == 5 + sizeof(float));
    auto other = loadBridged(kTestLevel);
    REQUIRE(other->processor().loadState(state));
    std::vector<float> ones(16, 1.0f), more(16, 1.0f);
    other->process(ones.data(), more.data(), 16, {});
    CHECK(ones[0] == 2.0f);
}

TEST_CASE("A VST 2 instrument in the bridge plays notes and keeps its chunk", "[plugin][vst2][bridge]")
{
    auto keys = loadBridged(kTestKeys);
    const auto state = keys->processor().saveState();
    REQUIRE(state.size() == 9);
    CHECK(state[5] == 't');
    CHECK(keys->processor().loadState(state));

    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(pluginChannel(keys, {1}));
    engine.play();
    const auto audio = render(engine, 0.25);
    CHECK(audio[0] == Approx(0.1f)); // the step's note
    CHECK(audio[6400 * 2] == 0.0f); // and ended
    CHECK(audio[1] == Approx(0.04f)); // told the song is in 4/4
}

TEST_CASE("A VST 2 editor in the bridge reports its size and edits", "[plugin][vst2][bridge][editor]")
{
    auto plugin = loadBridged(kTestLevel);
    vloom::AudioProcessor& level = plugin->processor();
    vloom::test::EditorRecorder recorder;
    REQUIRE(level.hasEditor());
    REQUIRE(level.openEditor(vloom::test::fakeWindow(), &recorder));
    REQUIRE(recorder.resizes.size() == 1);
    CHECK(recorder.resizes[0].width == 400);
    CHECK(level.editorSize().width == 400);
    CHECK(level.editorSize().height == 300);
    REQUIRE(recorder.edits.size() == 1);
    CHECK(recorder.edits[0].id == 0);
    CHECK(recorder.edits[0].value == Approx(0.25));
    std::vector<float> left(64, 1.0f);
    std::vector<float> right(64, 1.0f);
    plugin->process(left.data(), right.data(), 64, vloom::TransportInfo{});
    CHECK(left[0] == Approx(0.5f));
    level.closeEditor();
    CHECK_FALSE(level.isEditorOpen());
}

TEST_CASE("A bridged plugin does not allocate on the host's audio thread", "[plugin][vst2][bridge]")
{
    auto keys = loadBridged(kTestKeys);
    std::vector<float> l(256), r(256);
    const std::array<vloom::NoteEvent, 1> on{vloom::NoteEvent{0, vloom::NoteEvent::Type::On, 60, 1.0f}};
    const vloom::test::AllocationCounter counter;
    for (int block = 0; block < 50; ++block) {
        std::fill(l.begin(), l.end(), 0.0f); // an instrument adds to what is there
        keys->process(l.data(), r.data(), 256, {}, on);
    }
    CHECK(counter.count() == 0);
    CHECK(l[10] == Approx(0.1f));
}

TEST_CASE("A plugin that takes its bridge down leaves the host running", "[plugin][vst2][bridge]")
{
    auto keys = loadBridged(kTestKeys);
    std::vector<float> l(64, 0.25f), r(64, 0.25f);
    // Key 127 makes the test plugin end its process in the middle of a block.
    const std::array<vloom::NoteEvent, 1> fatal{vloom::NoteEvent{0, vloom::NoteEvent::Type::On, 127, 1.0f}};
    keys->process(l.data(), r.data(), 64, {}, fatal);
    CHECK(l[10] == 0.25f); // the audio as it came
    keys->process(l.data(), r.data(), 64, {});
    CHECK(l[10] == 0.25f);
    // Nothing is saved for it: an empty state must not replace the last good one.
    CHECK(keys->processor().saveState().empty());
    CHECK_FALSE(keys->processor().hasEditor());
    CHECK_FALSE(keys->processor().hasFailed()); // not for good: it is started again

    // The host's idle time starts a new bridge, and the plugin plays again.
    keys->processor().idle();
    const std::array<vloom::NoteEvent, 1> on{vloom::NoteEvent{0, vloom::NoteEvent::Type::On, 60, 1.0f}};
    std::fill(l.begin(), l.end(), 0.0f);
    keys->process(l.data(), r.data(), 64, {}, on);
    CHECK(l[10] == Approx(0.1f));
    CHECK_FALSE(keys->processor().saveState().empty());
}

TEST_CASE("A bridged plugin that is slow for one block comes back", "[plugin][vst2][bridge]")
{
    auto keys = loadBridged(kTestKeys);
    std::vector<float> l(64, 0.25f), r(64, 0.25f);
    // Key 126 makes the test plugin take 2.6 seconds over this block: longer than the host waits.
    const std::array<vloom::NoteEvent, 1> slow{vloom::NoteEvent{0, vloom::NoteEvent::Type::On, 126, 1.0f}};
    keys->process(l.data(), r.data(), 64, {}, slow);
    CHECK(l[10] == 0.25f); // the audio as it came
    CHECK_FALSE(keys->processor().hasFailed());
    keys->process(l.data(), r.data(), 64, {}); // still busy: passed through at once
    CHECK(l[10] == 0.25f);
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    keys->process(l.data(), r.data(), 64, {}); // its late answer is dropped
    std::fill(l.begin(), l.end(), 0.0f);
    keys->process(l.data(), r.data(), 64, {});
    CHECK(l[10] == Approx(0.1f)); // and it plays again: the key is still held
    CHECK_FALSE(keys->processor().hasFailed());
}

#endif

TEST_CASE("A plugin's name is kept if it is clean UTF-8 and made plain if not", "[plugin][vst2]")
{
    using vloom::plugin_files::printable;
    CHECK(printable("Reverb  ") == "Reverb");
    const std::string utf8 = "Caf\xC3\xA9 \xE0\xB8\x81"; // "Cafe" with an accent, and a Thai letter
    CHECK(printable(utf8) == utf8);
    CHECK(printable("Caf\xE9") == "Caf?"); // Latin-1: not UTF-8
    CHECK(printable("a\x07!") == "a?!"); // a control character
    CHECK(printable("\xC0\xAF") == "??"); // an overlong form
}

#if defined(_WIN32)
TEST_CASE("A module file tells what it is without being loaded", "[plugin][vst2]")
{
    using vloom::plugin_files::readModule;
    const auto level = readModule(kTestLevel, {"VSTPluginMain", "main"});
    CHECK(level.module);
    CHECK(level.exports);
#if defined(_M_X64)
    CHECK(level.machine == 0x8664);
#endif
    const auto clap = readModule(VLOOM_TEST_CLAP, {"VSTPluginMain", "main"});
    CHECK(clap.module);
    CHECK_FALSE(clap.exports); // a DLL, but no VST 2 plugin: it is never loaded
    CHECK(readModule(VLOOM_TEST_CLAP, {"clap_entry"}).exports);
    CHECK_FALSE(readModule(kTestLevel.parent_path() / "missing.dll", {"main"}).module);
    CHECK_FALSE(readModule(__FILE__, {"main"}).module); // a text file
}
#endif

TEST_CASE("A VST 2 plugin gets a sidechain and controls, and gives extra outputs, notes and programs",
    "[plugin][vst2][routing]")
{
    // tests/plugin/vst2/MultiPlugin.cpp
    auto plugin = load(VLOOM_TEST_VST2_MULTI);
    vloom::AudioProcessor& multi = plugin->processor();
    CHECK(multi.hasSidechain());
    CHECK(multi.extraOutputCount() == 2);
    CHECK(multi.makesNotes());
    REQUIRE(multi.programs() == std::vector<std::string>{"Soft", "Hard", "Wide"});
    CHECK(multi.currentProgram() == 0);
    CHECK(multi.setProgram(2));
    CHECK(multi.currentProgram() == 2);
    CHECK_FALSE(multi.setProgram(3));

    constexpr int kFrames = 64;
    std::vector<float> left(kFrames, 0.5f), right(kFrames, 0.5f), side(kFrames, 0.25f);
    std::array<std::vector<float>, 6> extra;
    std::array<float*, 6> extraPointers{};
    for (std::size_t i = 0; i < extra.size(); ++i) {
        extra[i].assign(kFrames, 9.0f); // stale: the host must not leave it
        extraPointers[i] = extra[i].data();
    }
    std::array<vloom::NoteEvent, 8> made{};
    int madeCount = 0;
    vloom::PluginInstance::Io io;
    io.sidechainLeft = side.data();
    io.sidechainRight = side.data();
    io.extraOutputs = extraPointers;
    io.notesOut = made;
    io.notesOutCount = &madeCount;
    REQUIRE(plugin->sendControl(vloom::ControlEvent::Type::PitchBend, 0, 1.0f));
    REQUIRE(plugin->sendControl(vloom::ControlEvent::Type::Controller, 1, 1.0f));
    const std::array<vloom::NoteEvent, 1> on{vloom::NoteEvent{5, vloom::NoteEvent::Type::On, 60, 1.0f}};
    plugin->process(left.data(), right.data(), kFrames, {}, on, io);

    CHECK(left[10] == Approx(0.5f * 0.75f)); // turned down by the sidechain
    CHECK(extra[0][10] == Approx(1.0f)); // output 2, left: the pitch bend, all the way up
    CHECK(extra[1][10] == Approx(1.0f)); // output 2, right: the mod wheel
    CHECK(extra[2][10] == Approx(0.3f)); // output 3
    CHECK(extra[4][10] == 0.0f); // an output it does not have: silence, not what was there
    REQUIRE(madeCount == 1); // the note it sent back, an octave up, at its frame
    CHECK(made[0].key == 72);
    CHECK(made[0].frame == 5);
    CHECK(made[0].type == vloom::NoteEvent::Type::On);

    // Without a sidechain it hears silence there: the sound passes.
    std::fill(left.begin(), left.end(), 0.5f);
    plugin->process(left.data(), right.data(), kFrames, {});
    CHECK(left[10] == Approx(0.5f));
}

#if defined(VLOOM_TEST_BRIDGE)
TEST_CASE("A bridged VST 2 plugin gets a sidechain and controls, and gives extra outputs, notes and programs",
    "[plugin][vst2][bridge][routing]")
{
    // The same plugin and the same expectations as in the host's own process.
    auto plugin = loadBridged(VLOOM_TEST_VST2_MULTI);
    vloom::AudioProcessor& multi = plugin->processor();
    CHECK(multi.hasSidechain());
    CHECK(multi.extraOutputCount() == 2);
    CHECK(multi.makesNotes());
    REQUIRE(multi.programs() == std::vector<std::string>{"Soft", "Hard", "Wide"});
    CHECK(multi.setProgram(1));
    CHECK(multi.currentProgram() == 1);
    CHECK_FALSE(multi.setProgram(3));

    constexpr int kFrames = 64;
    std::vector<float> left(kFrames, 0.5f), right(kFrames, 0.5f), side(kFrames, 0.25f);
    std::array<std::vector<float>, 6> extra;
    std::array<float*, 6> extraPointers{};
    for (std::size_t i = 0; i < extra.size(); ++i) {
        extra[i].assign(kFrames, 9.0f);
        extraPointers[i] = extra[i].data();
    }
    std::array<vloom::NoteEvent, 8> made{};
    int madeCount = 0;
    vloom::PluginInstance::Io io;
    io.sidechainLeft = side.data();
    io.sidechainRight = side.data();
    io.extraOutputs = extraPointers;
    io.notesOut = made;
    io.notesOutCount = &madeCount;
    REQUIRE(plugin->sendControl(vloom::ControlEvent::Type::PitchBend, 0, 1.0f));
    REQUIRE(plugin->sendControl(vloom::ControlEvent::Type::Controller, 1, 1.0f));
    const std::array<vloom::NoteEvent, 1> on{vloom::NoteEvent{5, vloom::NoteEvent::Type::On, 60, 1.0f}};
    plugin->process(left.data(), right.data(), kFrames, {}, on, io);

    CHECK(left[10] == Approx(0.5f * 0.75f));
    CHECK(extra[0][10] == Approx(1.0f));
    CHECK(extra[1][10] == Approx(1.0f));
    CHECK(extra[2][10] == Approx(0.3f));
    CHECK(extra[4][10] == 0.0f);
    REQUIRE(madeCount == 1);
    CHECK(made[0].key == 72);
    CHECK(made[0].frame == 5);
}
#endif
