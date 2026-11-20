// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// The optional capabilities of the built-ins (ADR 0008): sidechain, tempo,
// latency that changes, sample slots, choice labels and meters.
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/mixer/MixerState.hpp>
#include <vloom/engine/plugin/BuiltinProcessors.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/render/WavWriter.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// The id of the parameter named `name`; fails the test if there is none.
std::uint32_t idOf(const vloom::AudioProcessor& processor, const std::string& name)
{
    for (const vloom::ParameterInfo& parameter : processor.parameters())
        if (parameter.name == name)
            return parameter.id;
    FAIL("no parameter " << name);
    return 0;
}

const vloom::ParameterInfo& parameterNamed(
    const std::vector<vloom::ParameterInfo>& list, const std::string& name)
{
    const auto it = std::find_if(
        list.begin(), list.end(), [&](const vloom::ParameterInfo& p) { return p.name == name; });
    REQUIRE(it != list.end());
    return *it;
}

std::shared_ptr<vloom::PluginInstance> activeInstance(std::string_view id)
{
    auto processor = vloom::createBuiltinProcessor(id);
    REQUIRE(processor != nullptr);
    auto instance = std::make_shared<vloom::PluginInstance>(std::move(processor));
    REQUIRE(instance->activate(kRate, vloom::kMaxBlockFrames));
    return instance;
}

/// A temporary folder, removed with what it holds at the end of the test.
struct TempFolder {
    std::filesystem::path path;
    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("vloom_builtin_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
};

/// Writes `seconds` of a sine at `hz` (mono, amplitude 0.5) at `rate` to `path`.
void writeSine(const std::filesystem::path& path, double hz, double seconds, int rate)
{
    std::vector<float> samples(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.5f
            * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    std::string error;
    REQUIRE(vloom::writeWavFloat32(path.string(), samples, 1, rate, &error));
}

/// Rising zero crossings of `samples` per second at kRate: its pitch.
double pitchOf(std::span<const float> samples)
{
    int rising = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
            ++rising;
    return rising * kRate / static_cast<double>(samples.size());
}

/// Plays `key` on `instance` (an instrument) for `seconds`; returns the left channel.
std::vector<float> playKey(vloom::PluginInstance& instance, int key, double seconds)
{
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> out;
    const std::array on{
        vloom::NoteEvent{0, vloom::NoteEvent::Type::On, static_cast<std::int16_t>(key), 1.0f}};
    std::vector<float> left(512);
    std::vector<float> right(512);
    for (std::size_t done = 0; done < frames; done += 512) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        instance.process(left.data(), right.data(), 512, vloom::TransportInfo{},
            done == 0 ? std::span<const vloom::NoteEvent>(on) : std::span<const vloom::NoteEvent>());
        out.insert(out.end(), left.begin(), left.end());
    }
    return out;
}

/// An instrument for the tests that always sounds `level` on both channels.
class Drone final : public vloom::AudioProcessor {
public:
    explicit Drone(float level)
        : level_(level)
    {
    }
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.drone", "Drone", "Tests", "1", vloom::PluginInfo::Kind::Instrument};
    }
    bool activate(double, int) override { return true; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        for (int ch = 0; ch < context.numChannels; ++ch)
            for (int i = 0; i < context.numFrames; ++i)
                context.channels[ch][i] += level_;
    }

private:
    float level_;
};

vloom::ChannelState droneOn(int insert, float level)
{
    auto plugin = std::make_shared<vloom::PluginInstance>(std::make_unique<Drone>(level));
    REQUIRE(plugin->activate(kRate, vloom::kMaxBlockFrames));
    vloom::ChannelState c;
    c.instrument = vloom::Instrument::Plugin;
    c.plugin = std::move(plugin);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    c.insert = insert;
    return c;
}

void prepare(vloom::Engine& engine, double tempo = 120.0)
{
    engine.prepare(kRate, 256);
    engine.setTempo(tempo);
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

TEST_CASE("The engine lists all twenty built-ins with a category", "[plugin][builtin]")
{
    const std::set<std::string> effects{"vloom.gain", "vloom.eq", "vloom.compressor", "vloom.limiter",
        "vloom.gate", "vloom.delay", "vloom.reverb", "vloom.saturator", "vloom.utility", "vloom.filter",
        "vloom.chorus", "vloom.flanger", "vloom.phaser", "vloom.tremolo", "vloom.deesser", "vloom.transient",
        "vloom.bitcrusher"};
    const std::set<std::string> instruments{"vloom.synth", "vloom.sampler", "vloom.drums"};
    const std::set<std::string> categories{"Dynamics", "EQ/Filter", "Delay/Reverb", "Modulation",
        "Distortion", "Utility", "Synth", "Sampler", "Drums"};

    const auto list = vloom::builtinProcessors();
    CHECK(list.size() == 20);
    std::set<std::string> listed;
    for (const vloom::PluginInfo& info : list) {
        INFO(info.id);
        listed.insert(info.id);
        CHECK_FALSE(info.name.empty());
        CHECK(categories.count(info.category) == 1);
        const bool instrument = instruments.count(info.id) == 1;
        CHECK(instrument == (info.kind == vloom::PluginInfo::Kind::Instrument));
        CHECK(instrument
            == (info.category == "Synth" || info.category == "Sampler" || info.category == "Drums"));
        const auto processor = vloom::createBuiltinProcessor(info.id);
        REQUIRE(processor != nullptr);
        CHECK(processor->info().category == info.category);
    }
    std::set<std::string> all = effects;
    all.insert(instruments.begin(), instruments.end());
    CHECK(listed == all);
    CHECK(vloom::createBuiltinProcessor("vloom.compressor")->info().category == "Dynamics");
    CHECK(vloom::createBuiltinProcessor("vloom.reverb")->info().category == "Delay/Reverb");
    CHECK(vloom::createBuiltinProcessor("vloom.drums")->info().category == "Drums");
}

TEST_CASE("Only the compressor and the gate have a sidechain", "[plugin][builtin]")
{
    for (const vloom::PluginInfo& info : vloom::builtinProcessors()) {
        INFO(info.id);
        const bool keyed = info.id == "vloom.compressor" || info.id == "vloom.gate";
        CHECK(vloom::createBuiltinProcessor(info.id)->hasSidechain() == keyed);
    }
}

TEST_CASE("A mixer sidechain send keys the built-in compressor", "[plugin][builtin][routing]")
{
    // Insert 1: a quiet drone (0.1, -20 dB) through a compressor whose
    // threshold it stays under. Insert 2: a loud drone sent only into insert
    // 1's sidechain, or not at all.
    const auto run = [](bool keyed) {
        vloom::Engine engine;
        prepare(engine);
        auto rack = std::make_unique<vloom::SequencerState>();
        rack->channels.push_back(droneOn(1, 0.1f));
        rack->channels.push_back(droneOn(2, 0.8f));
        engine.setSequencerState(std::move(rack));
        auto compressor = activeInstance("vloom.compressor");
        REQUIRE(compressor->setParameter(idOf(compressor->processor(), "Threshold"), -12.0));
        REQUIRE(compressor->setParameter(idOf(compressor->processor(), "Ratio"), 10.0));
        REQUIRE(compressor->setParameter(idOf(compressor->processor(), "Knee"), 0.0));
        auto mixer = std::make_unique<vloom::MixerState>();
        mixer->inserts.resize(2);
        mixer->inserts[0].effects = {compressor};
        mixer->inserts[1].output = -1; // only its sidechain send, if any, is heard
        if (keyed) {
            vloom::Send send;
            send.target = 1;
            send.gain = 1.0f;
            send.sidechain = true;
            mixer->inserts[1].sends = {send};
        }
        engine.setMixerState(std::move(mixer));
        engine.play();
        const auto audio = render(engine, 0.5);
        struct Result {
            float level;
            float reduction;
        };
        return Result{audio[audio.size() - 2], compressor->processor().gainReductionDb().value_or(-1.0f)};
    };
    const auto alone = run(false);
    const auto keyed = run(true);
    // Under the threshold on its own: untouched, nothing reduced.
    CHECK(alone.level == Approx(0.1f).margin(1e-3));
    CHECK(alone.reduction == Approx(0.0f).margin(0.05));
    // Keyed by the loud drone (-1.9 dB, 10 dB over): turned down by about 9 dB.
    CHECK(keyed.reduction > 6.0f);
    CHECK(keyed.level < 0.05f);
    CHECK(keyed.level > 0.0f);
}

TEST_CASE("A built-in gate opens for its key, not its own input", "[plugin][builtin]")
{
    auto gate = activeInstance("vloom.gate");
    std::vector<float> left(512);
    std::vector<float> right(512);
    std::vector<float> keyLeft(512, 0.5f);
    std::vector<float> keyRight(512, 0.5f);
    const auto block = [&](bool keyed) {
        std::fill(left.begin(), left.end(), 0.001f); // -60 dB: under any threshold
        std::fill(right.begin(), right.end(), 0.001f);
        vloom::PluginInstance::Io io;
        if (keyed) {
            io.sidechainLeft = keyLeft.data();
            io.sidechainRight = keyRight.data();
        }
        const vloom::test::AllocationCounter counter;
        gate->process(left.data(), right.data(), 512, vloom::TransportInfo{}, {}, io);
        CHECK(counter.count() == 0);
        return left.back();
    };
    for (int i = 0; i < 100; ++i)
        (void)block(false);
    const float closed = block(false);
    for (int i = 0; i < 100; ++i)
        (void)block(true);
    const float open = block(true);
    CHECK(open == Approx(0.001f).margin(1e-5));
    CHECK(closed < 0.5f * open);
}

TEST_CASE("A tempo-synced built-in delay follows the transport's tempo", "[plugin][builtin][tempo]")
{
    auto delay = activeInstance("vloom.delay");
    const vloom::AudioProcessor& processor = delay->processor();
    REQUIRE(delay->setParameter(idOf(processor, "Sync"), 1.0));
    REQUIRE(delay->setParameter(idOf(processor, "Note"), 5.0)); // 1/8: half a beat
    REQUIRE(delay->setParameter(idOf(processor, "Feedback"), 0.0));
    REQUIRE(delay->setParameter(idOf(processor, "Mix"), 100.0));
    REQUIRE(parameterNamed(processor.parameters(), "Note").valueLabels.at(5) == "1/8");

    // Where an impulse comes back, at `tempo`.
    const auto echoAt = [&](double tempo) {
        vloom::TransportInfo transport;
        transport.tempo = tempo;
        std::vector<float> left(512);
        std::vector<float> right(512);
        const auto block = [&](bool impulse) {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            if (impulse)
                left[0] = right[0] = 1.0f;
            delay->process(left.data(), right.data(), 512, transport);
        };
        for (int i = 0; i < 100; ++i) // over a second: any glide to the new time is done, the line empty
            block(false);
        std::vector<float> out;
        for (int i = 0; i < 100; ++i) {
            block(i == 0);
            out.insert(out.end(), left.begin(), left.end());
        }
        const auto loudest = std::max_element(
            out.begin(), out.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
        return static_cast<int>(loudest - out.begin());
    };
    CHECK(std::abs(echoAt(120.0) - 12000) <= 4); // 250 ms
    CHECK(std::abs(echoAt(60.0) - 24000) <= 4); // the tempo halves: twice as late
    CHECK(std::abs(echoAt(150.0) - 9600) <= 4);
}

TEST_CASE("A built-in limiter's latency change reaches delay compensation", "[plugin][builtin][pdc]")
{
    vloom::Engine engine;
    prepare(engine);
    auto limiter = activeInstance("vloom.limiter");
    const int before = limiter->latencyFrames();
    CHECK(before == 96 + 4); // 2 ms look-ahead, true peak on
    auto mixer = std::make_unique<vloom::MixerState>();
    mixer->inserts.resize(1);
    mixer->inserts[0].effects = {limiter};
    engine.setMixerState(std::move(mixer));
    CHECK_FALSE(engine.updatePluginLatency());

    REQUIRE(limiter->setParameter(idOf(limiter->processor(), "Lookahead"), 5.0));
    REQUIRE(limiter->setParameter(idOf(limiter->processor(), "True Peak"), 0.0));
    (void)render(engine, 0.02); // the audio thread applies them
    CHECK(limiter->processor().latencyFrames() == 240);
    REQUIRE(engine.updatePluginLatency());
    CHECK(limiter->latencyFrames() == 240);
    CHECK_FALSE(engine.updatePluginLatency()); // once

    // The saturator's oversampling too.
    auto saturator = activeInstance("vloom.saturator");
    const int fourTimes = saturator->latencyFrames();
    REQUIRE(saturator->setParameter(idOf(saturator->processor(), "Oversampling"), 0.0));
    std::vector<float> left(256);
    std::vector<float> right(256);
    saturator->process(left.data(), right.data(), 256, vloom::TransportInfo{});
    REQUIRE(saturator->updateLatency());
    CHECK(saturator->latencyFrames() != fourTimes);
    CHECK(saturator->latencyFrames() > 0);
}

TEST_CASE("Stepped choices of the built-ins have labels", "[plugin][builtin]")
{
    const auto delay = vloom::createBuiltinProcessor("vloom.delay")->parameters();
    CHECK(parameterNamed(delay, "Mode").valueLabels == std::vector<std::string>{"Stereo", "Ping-pong"});
    CHECK(parameterNamed(delay, "Note").valueLabels.size() == 15);
    CHECK(parameterNamed(delay, "Feedback").valueLabels.empty());
    CHECK(parameterNamed(delay, "Sync").valueLabels.empty()); // a switch

    const auto chorus = vloom::createBuiltinProcessor("vloom.chorus")->parameters();
    const auto& note = parameterNamed(chorus, "Note");
    REQUIRE(note.valueLabels.size() == 20);
    CHECK(note.valueLabels[9] == "1/4");
    CHECK(note.valueLabels[10] == "1/8 D");

    const auto compressor = vloom::createBuiltinProcessor("vloom.compressor")->parameters();
    CHECK(parameterNamed(compressor, "Detector").valueLabels == std::vector<std::string>{"Peak", "RMS"});
    const auto filter = vloom::createBuiltinProcessor("vloom.filter")->parameters();
    CHECK(parameterNamed(filter, "Type").valueLabels.size() == 4);
    const auto sampler = vloom::createBuiltinProcessor("vloom.sampler")->parameters();
    const auto& root = parameterNamed(sampler, "Root Key");
    REQUIRE(root.valueLabels.size() == 128);
    CHECK(root.valueLabels[60] == "C3");
    CHECK(root.valueLabels[36] == "C1");
    const auto drums = vloom::createBuiltinProcessor("vloom.drums")->parameters();
    CHECK(parameterNamed(drums, "Pad 2 Choke Group").valueLabels.front() == "Off");

    // Every label list names each whole value, nothing more.
    for (const vloom::PluginInfo& info : vloom::builtinProcessors())
        for (const vloom::ParameterInfo& p : vloom::createBuiltinProcessor(info.id)->parameters()) {
            INFO(info.id << " " << p.name);
            if (!p.valueLabels.empty()) {
                CHECK(p.stepped);
                CHECK(static_cast<double>(p.valueLabels.size()) == p.maxValue - p.minValue + 1.0);
            }
        }
}

TEST_CASE("Dynamics built-ins report their gain reduction", "[plugin][builtin]")
{
    auto compressor = activeInstance("vloom.compressor");
    CHECK(compressor->processor().gainReductionDb() == 0.0f);
    std::vector<float> left(512);
    std::vector<float> right(512);
    for (int i = 0; i < 50; ++i) {
        std::fill(left.begin(), left.end(), 0.9f);
        std::fill(right.begin(), right.end(), 0.9f);
        compressor->process(left.data(), right.data(), 512, vloom::TransportInfo{});
    }
    CHECK(compressor->processor().gainReductionDb().value_or(0.0f) > 3.0f);
    for (const char* id : {"vloom.gate", "vloom.limiter", "vloom.deesser"})
        CHECK(vloom::createBuiltinProcessor(id)->gainReductionDb().has_value());
    CHECK_FALSE(vloom::createBuiltinProcessor("vloom.gain")->gainReductionDb().has_value());
}

TEST_CASE(
    "The built-in sampler plays a WAV file at its own pitch on the root key", "[plugin][builtin][sample]")
{
    const TempFolder folder;
    const auto file = folder.path / "sine.wav";
    writeSine(file, 441.0, 1.0, 44100); // another rate than the engine's

    auto sampler = activeInstance("vloom.sampler");
    vloom::AudioProcessor& processor = sampler->processor();
    REQUIRE(processor.sampleSlotCount() == 1);
    CHECK(processor.sampleSlot(0).name == "Sample");
    CHECK_FALSE(processor.sampleSlot(0).loaded());
    CHECK(playKey(*sampler, 60, 0.1) == std::vector<float>(5120, 0.0f)); // nothing to play yet

    std::string error;
    REQUIRE(processor.loadSample(0, file.string(), &error));
    CHECK(error.empty());
    const vloom::SampleSlotInfo info = processor.sampleSlot(0);
    CHECK(info.path == file.string());
    CHECK(info.loaded());
    CHECK(info.numChannels == 1);
    CHECK(info.frames == 44100);
    CHECK(info.sampleRate == 44100.0);
    CHECK(info.error.empty());

    // Root key (60 by default): the file's own 441 Hz, resampled to 48 kHz.
    const auto root = playKey(*sampler, 60, 0.5);
    CHECK(pitchOf(std::span<const float>(root).subspan(2400, 19200)) == Approx(441.0).epsilon(0.02));
    // An octave up.
    processor.reset();
    const auto octave = playKey(*sampler, 72, 0.5);
    CHECK(pitchOf(std::span<const float>(octave).subspan(2400, 19200)) == Approx(882.0).epsilon(0.02));

    // Bad slots and files are refused; the slot keeps its sample.
    CHECK_FALSE(processor.loadSample(1, file.string(), &error));
    CHECK_FALSE(processor.loadSample(0, (folder.path / "missing.wav").string(), &error));
    CHECK_FALSE(error.empty());
    CHECK(processor.sampleSlot(0).path == file.string());
    CHECK(processor.sampleSlot(0).loaded());
    processor.idle(); // releases nothing in use

    REQUIRE(processor.clearSample(0));
    CHECK_FALSE(processor.sampleSlot(0).loaded());
    CHECK(processor.sampleSlot(0).path.empty());
    CHECK_FALSE(processor.clearSample(3));

    // Effects have no slots.
    auto gain = vloom::createBuiltinProcessor("vloom.gain");
    CHECK(gain->sampleSlotCount() == 0);
    CHECK_FALSE(gain->loadSample(0, file.string(), &error));
}

TEST_CASE("A built-in's state keeps its sample paths and parameters", "[plugin][builtin][sample]")
{
    const TempFolder folder;
    const auto kick = folder.path / "kick.wav";
    const auto snare = folder.path / "snare.wav";
    writeSine(kick, 60.0, 0.2, 48000);
    writeSine(snare, 200.0, 0.1, 22050);

    auto drums = vloom::createBuiltinProcessor("vloom.drums");
    REQUIRE(drums->sampleSlotCount() == 16);
    CHECK(drums->sampleSlot(0).name == "Pad 1");
    CHECK(drums->sampleSlot(15).name == "Pad 16");
    REQUIRE(drums->loadSample(0, kick.string()));
    REQUIRE(drums->loadSample(2, snare.string()));
    const std::uint32_t tune = idOf(*drums, "Pad 3 Tune");
    REQUIRE(drums->parameterChangedByHost(tune, 5.0));
    const std::vector<std::uint8_t> state = drums->saveState();

    auto restored = vloom::createBuiltinProcessor("vloom.drums");
    REQUIRE(restored->loadState(state));
    CHECK(restored->parameterValue(tune) == 5.0);
    CHECK(restored->sampleSlot(0).path == kick.string());
    CHECK(restored->sampleSlot(0).loaded());
    CHECK(restored->sampleSlot(2).path == snare.string());
    CHECK(restored->sampleSlot(2).sampleRate == 22050.0);
    CHECK_FALSE(restored->sampleSlot(1).loaded());
    CHECK(restored->saveState() == state);

    // It plays what it restored: pad 1 is key 36.
    auto instance = std::make_shared<vloom::PluginInstance>(std::move(restored));
    REQUIRE(instance->activate(kRate, vloom::kMaxBlockFrames));
    const auto hit = playKey(*instance, 36, 0.1);
    CHECK(std::any_of(hit.begin(), hit.end(), [](float s) { return std::abs(s) > 0.1f; }));

    // A file gone missing: the state still loads, the path is kept and the
    // slot says why it is silent; saving again keeps the path.
    std::filesystem::remove(snare);
    auto missing = vloom::createBuiltinProcessor("vloom.drums");
    REQUIRE(missing->loadState(state));
    CHECK(missing->sampleSlot(0).loaded());
    const vloom::SampleSlotInfo gone = missing->sampleSlot(2);
    CHECK(gone.path == snare.string());
    CHECK_FALSE(gone.loaded());
    CHECK_FALSE(gone.error.empty());
    CHECK(missing->parameterValue(tune) == 5.0);
    CHECK(missing->saveState() == state);

    // A state without paths empties the slots. Broken path data is refused.
    auto params = state;
    params.resize(4 + 4 + drums->parameters().size() * 12);
    REQUIRE(missing->loadState(params));
    CHECK(missing->sampleSlot(0).path.empty());
    CHECK_FALSE(missing->sampleSlot(0).loaded());
    auto cut = state;
    cut.pop_back();
    CHECK_FALSE(missing->loadState(cut));
    auto extra = state;
    extra.push_back(0);
    CHECK_FALSE(missing->loadState(extra));
    // Effects without slots take no trailing data.
    auto gain = vloom::createBuiltinProcessor("vloom.gain");
    auto gainState = gain->saveState();
    gainState.insert(gainState.end(), {'S', 'M', 'P', '1', 0, 0, 0, 0});
    CHECK_FALSE(gain->loadState(gainState));
}

TEST_CASE("Sample files can be read through a resolver", "[plugin][builtin][sample]")
{
    const TempFolder folder;
    const auto wav = folder.path / "decoded.wav";
    writeSine(wav, 300.0, 0.1, 48000);
    auto sampler = vloom::createBuiltinProcessor("vloom.sampler");
    sampler->setSampleFileResolver([wav](const std::string& path, std::string* error) -> std::string {
        if (path == "song.mp3")
            return wav.string();
        if (error != nullptr)
            *error = "cannot decode " + path;
        return {};
    });
    REQUIRE(sampler->loadSample(0, "song.mp3"));
    CHECK(sampler->sampleSlot(0).path == "song.mp3"); // the original, for the project
    CHECK(sampler->sampleSlot(0).loaded());
    std::string error;
    CHECK_FALSE(sampler->loadSample(0, "other.ogg", &error));
    CHECK(error == "cannot decode other.ogg");

    // Restoring goes through it too.
    const auto state = sampler->saveState();
    auto restored = vloom::createBuiltinProcessor("vloom.sampler");
    REQUIRE(restored->loadState(state));
    CHECK_FALSE(restored->sampleSlot(0).loaded()); // no resolver: "song.mp3" is no file
    CHECK(restored->sampleSlot(0).path == "song.mp3");
    restored->setSampleFileResolver([wav](const std::string&, std::string*) { return wav.string(); });
    REQUIRE(restored->loadState(state));
    CHECK(restored->sampleSlot(0).loaded());
}
