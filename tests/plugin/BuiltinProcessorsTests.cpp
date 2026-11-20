// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/plugin/BuiltinProcessors.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/render/WavWriter.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr std::uint32_t kGain = 0;
constexpr float kMinus6Db = 0.501187f;

/// Processes `frames` of ones with `events`; returns the left channel.
std::vector<float> runOnes(
    vloom::AudioProcessor& processor, int frames, std::span<const vloom::ParameterEvent> events = {})
{
    std::vector<float> left(static_cast<std::size_t>(frames), 1.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 1.0f);
    std::array<float*, 2> channels{left.data(), right.data()};
    vloom::ProcessContext context;
    context.channels = channels.data();
    context.numChannels = 2;
    context.numFrames = frames;
    context.parameters = events;
    processor.process(context);
    CHECK(left == right);
    return left;
}

std::unique_ptr<vloom::AudioProcessor> activeGain()
{
    auto gain = vloom::createBuiltinProcessor("vloom.gain");
    REQUIRE(gain != nullptr);
    REQUIRE(gain->activate(kRate, 1024));
    return gain;
}

/// A state with the given (id, value) entries.
std::vector<std::uint8_t> stateOf(std::initializer_list<std::pair<std::uint32_t, double>> entries)
{
    std::vector<std::uint8_t> state{'V', 'L', 'B', '1'};
    const auto put = [&](std::uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            state.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    put(entries.size(), 4);
    for (const auto& [id, value] : entries) {
        put(id, 4);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        put(bits, 8);
    }
    return state;
}

} // namespace

TEST_CASE("The engine lists its built-in processors and creates them by id", "[plugin][builtin]")
{
    const auto list = vloom::builtinProcessors();
    REQUIRE_FALSE(list.empty());
    bool hasGain = false;
    for (const auto& info : list) {
        CHECK(info.vendor == "Vloom Audio");
        auto processor = vloom::createBuiltinProcessor(info.id);
        REQUIRE(processor != nullptr);
        CHECK(processor->info().id == info.id);
        hasGain = hasGain || info.id == "vloom.gain";
    }
    CHECK(hasGain);
    CHECK(vloom::createBuiltinProcessor("vloom.nothing") == nullptr);
    CHECK(vloom::createBuiltinProcessor("") == nullptr);

    auto gain = vloom::createBuiltinProcessor("vloom.gain");
    CHECK(gain->info().kind == vloom::PluginInfo::Kind::Effect);
    REQUIRE(gain->parameters().size() == 1);
    CHECK(gain->parameters()[0].unit == "dB");
    CHECK(gain->latencyFrames() == 0);
    CHECK_FALSE(gain->activate(0.0, 512));
    CHECK_FALSE(gain->activate(kRate, 0));
}

TEST_CASE("A built-in applies a parameter change at its frame", "[plugin][builtin]")
{
    auto gain = activeGain();
    const std::array events{vloom::ParameterEvent{100, kGain, -6.0}};
    const auto audio = runOnes(*gain, 600, events);
    for (int i = 0; i < 100; ++i)
        REQUIRE(audio[static_cast<std::size_t>(i)] == 1.0f); // before the change
    CHECK(audio[100] < 1.0f); // the glide starts on its frame
    CHECK(audio[599] == Approx(kMinus6Db).margin(1e-5));
}

TEST_CASE("A built-in clamps values and ignores NaN and unknown parameters", "[plugin][builtin]")
{
    auto gain = activeGain();
    const std::array events{
        vloom::ParameterEvent{0, kGain, 1000.0},
        vloom::ParameterEvent{1, kGain, std::numeric_limits<double>::quiet_NaN()},
        vloom::ParameterEvent{2, 42, -60.0},
        vloom::ParameterEvent{-5, kGain, 1000.0}, // out of order and range: at the current frame
    };
    (void)runOnes(*gain, 400, events);
    const auto state = gain->saveState();
    CHECK(state == stateOf({{kGain, 12.0}})); // clamped to the maximum
}

TEST_CASE("A built-in's state saves and restores its parameters", "[plugin][builtin]")
{
    auto gain = activeGain();
    CHECK(gain->saveState() == stateOf({{kGain, 0.0}})); // the defaults

    // Loaded while active: the audio thread takes it at its next block.
    REQUIRE(gain->loadState(stateOf({{kGain, -6.0}})));
    CHECK(gain->saveState() == stateOf({{kGain, -6.0}}));
    (void)runOnes(*gain, 300);
    CHECK(runOnes(*gain, 4).front() == Approx(kMinus6Db).margin(1e-5));

    // Unknown ids (a newer version) are skipped; NaN is ignored; values are clamped.
    REQUIRE(gain->loadState(stateOf({{99, 1.0}, {kGain, std::numeric_limits<double>::quiet_NaN()}})));
    CHECK(gain->saveState() == stateOf({{kGain, -6.0}}));
    REQUIRE(gain->loadState(stateOf({{kGain, -500.0}})));
    CHECK(gain->saveState() == stateOf({{kGain, -60.0}}));

    // Broken state is refused and changes nothing.
    auto wrongMagic = stateOf({{kGain, 0.0}});
    wrongMagic[0] = 'X';
    CHECK_FALSE(gain->loadState(wrongMagic));
    auto truncated = stateOf({{kGain, 0.0}});
    truncated.pop_back();
    CHECK_FALSE(gain->loadState(truncated));
    auto hugeCount = stateOf({});
    hugeCount[4] = hugeCount[5] = hugeCount[6] = hugeCount[7] = 0xff;
    CHECK_FALSE(gain->loadState(hugeCount));
    CHECK_FALSE(gain->loadState({}));
    CHECK(gain->saveState() == stateOf({{kGain, -60.0}}));
}

TEST_CASE("A built-in loaded before activation starts at its state", "[plugin][builtin]")
{
    auto gain = vloom::createBuiltinProcessor("vloom.gain");
    REQUIRE(gain->loadState(stateOf({{kGain, -6.0}})));
    REQUIRE(gain->activate(kRate, 512));
    CHECK(runOnes(*gain, 1).front() == Approx(kMinus6Db).margin(1e-5)); // no glide
}

TEST_CASE("A built-in does not allocate on the audio thread", "[plugin][builtin]")
{
    auto gain = activeGain();
    std::vector<float> left(512, 1.0f);
    std::vector<float> right(512, 1.0f);
    std::array<float*, 2> channels{left.data(), right.data()};
    const std::array events{
        vloom::ParameterEvent{10, kGain, -6.0},
        vloom::ParameterEvent{200, kGain, 3.0},
        vloom::ParameterEvent{300, kGain, std::numeric_limits<double>::quiet_NaN()},
        vloom::ParameterEvent{400, 42, 1.0},
    };
    vloom::ProcessContext context;
    context.channels = channels.data();
    context.numChannels = 2;
    context.numFrames = 512;

    long long allocations = 0;
    for (int block = 0; block < 50; ++block) {
        if (block % 10 == 5) // control thread: outside the count
            REQUIRE(gain->loadState(stateOf({{kGain, -0.5 * block}})));
        context.parameters = block % 2 == 0 ? std::span<const vloom::ParameterEvent>(events)
                                            : std::span<const vloom::ParameterEvent>();
        const vloom::test::AllocationCounter counter;
        gain->process(context); // block splits, a pending loadState(), glides
        gain->reset();
        allocations += counter.count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("The built-in EQ is listed, created and shapes the sound", "[plugin][builtin]")
{
    bool listed = false;
    for (const vloom::PluginInfo& info : vloom::builtinProcessors())
        listed = listed || (info.id == "vloom.eq" && info.name == "EQ");
    CHECK(listed);

    auto eq = vloom::createBuiltinProcessor("vloom.eq");
    REQUIRE(eq != nullptr);
    CHECK(eq->info().kind == vloom::PluginInfo::Kind::Effect);
    CHECK(eq->parameters().size() == 14);
    CHECK_FALSE(eq->hasEditor());
    REQUIRE(eq->activate(kRate, 1024));

    // At its defaults: unchanged. With the low cut on, steady DC goes away.
    CHECK(runOnes(*eq, 256).back() == 1.0f);
    constexpr std::uint32_t kLowCutOn = 0;
    const std::array events{vloom::ParameterEvent{0, kLowCutOn, 1.0}};
    (void)runOnes(*eq, 1024, events);
    float last = 1.0f;
    for (int block = 0; block < 40; ++block)
        last = runOnes(*eq, 1024).back();
    CHECK(std::abs(last) < 1.0e-3f);
}

TEST_CASE("A built-in reports its values, also those set while no audio runs", "[plugin][builtin]")
{
    auto processor = vloom::createBuiltinProcessor("vloom.gain");
    REQUIRE(processor != nullptr);
    CHECK(processor->parameterValue(kGain) == 0.0); // the default
    CHECK_FALSE(processor->parameterValue(99).has_value());

    vloom::PluginInstance instance(std::move(processor));
    REQUIRE(instance.activate(kRate, 1024));
    // More changes than a queue would hold, and no block to drain it: a
    // built-in takes each at once.
    for (int i = 0; i <= static_cast<int>(vloom::PluginInstance::kMaxQueuedParameters) + 10; ++i)
        CHECK(instance.setParameter(kGain, -0.01 * i));
    const double last = -0.01 * (static_cast<int>(vloom::PluginInstance::kMaxQueuedParameters) + 10);
    CHECK(instance.processor().parameterValue(kGain) == last);
    CHECK(instance.processor().saveState() == stateOf({{kGain, last}}));

    CHECK(instance.setParameter(kGain, 1000.0));
    CHECK(instance.processor().parameterValue(kGain) == 12.0); // clamped
    CHECK_FALSE(instance.setParameter(kGain, std::numeric_limits<double>::quiet_NaN()));
    CHECK(instance.processor().parameterValue(kGain) == 12.0);

    // The audio thread takes the recorded value with its next block.
    std::vector<float> left(1024, 1.0f);
    std::vector<float> right(1024, 1.0f);
    CHECK(instance.setParameter(kGain, -6.0));
    for (int block = 0; block < 3; ++block) {
        std::fill(left.begin(), left.end(), 1.0f);
        std::fill(right.begin(), right.end(), 1.0f);
        instance.process(left.data(), right.data(), 1024, vloom::TransportInfo{});
    }
    CHECK(left.back() == Catch::Approx(kMinus6Db).margin(1e-5));
}

TEST_CASE("No built-in allocates on the audio thread", "[plugin][builtin]")
{
    // A sample for the built-ins with sample slots.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path folder
        = std::filesystem::temp_directory_path() / ("vloom_builtin_alloc_" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    const std::string sample = (folder / "tone.wav").string();
    {
        std::vector<float> tone(4410 * 2);
        for (std::size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i / 2)));
        REQUIRE(vloom::writeWavFloat32(sample, tone, 2, 44100));
    }

    for (const vloom::PluginInfo& info : vloom::builtinProcessors()) {
        INFO(info.id);
        auto processor = vloom::createBuiltinProcessor(info.id);
        REQUIRE(processor != nullptr);
        for (int slot = 0; slot < processor->sampleSlotCount(); ++slot)
            REQUIRE(processor->loadSample(slot, sample));
        REQUIRE(processor->activate(kRate, 1024));
        const std::vector<vloom::ParameterInfo> parameters = processor->parameters();
        const std::vector<std::uint8_t> defaults = processor->saveState();

        // Every parameter moves to an end of its range at some frame.
        std::vector<vloom::ParameterEvent> up;
        std::vector<vloom::ParameterEvent> down;
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const int frame = static_cast<int>(i * 30);
            up.push_back({frame, parameters[i].id, parameters[i].maxValue});
            down.push_back({frame, parameters[i].id, parameters[i].minValue});
        }
        std::vector<float> left(512, 0.25f);
        std::vector<float> right(512, -0.25f);
        std::array<float*, 2> channels{left.data(), right.data()};
        // A key for the dynamics, every other block.
        std::vector<float> keyLeft(512, 0.8f);
        std::vector<float> keyRight(512, -0.8f);
        const std::array<const float*, 2> key{keyLeft.data(), keyRight.data()};
        // Notes for the instruments: the drum sampler's first pads, a chord.
        const std::array notes{vloom::NoteEvent{5, vloom::NoteEvent::Type::On, 36, 1.0f},
            vloom::NoteEvent{40, vloom::NoteEvent::Type::On, 38, 0.7f},
            vloom::NoteEvent{90, vloom::NoteEvent::Type::On, 60, 0.9f},
            vloom::NoteEvent{300, vloom::NoteEvent::Type::Off, 60, 0.0f},
            vloom::NoteEvent{310, vloom::NoteEvent::Type::Off, 36, 0.0f}};
        vloom::ProcessContext context;
        context.channels = channels.data();
        context.numChannels = 2;
        context.numFrames = 512;

        const auto readoutCount = static_cast<int>(processor->readouts().size());
        long long allocations = 0;
        for (int block = 0; block < 60; ++block) {
            if (block % 10 == 5) { // control thread: outside the count
                REQUIRE(processor->loadState(defaults)); // the samples are loaded again too
                if (!parameters.empty())
                    (void)processor->parameterChangedByHost(parameters[0].id, parameters[0].maxValue);
                for (int slot = 0; slot < processor->sampleSlotCount(); slot += 3)
                    REQUIRE(processor->loadSample(slot, sample)); // replaced while notes play
                processor->idle();
            }
            context.parameters = block % 3 == 0 ? std::span<const vloom::ParameterEvent>(up)
                : block % 3 == 1                ? std::span<const vloom::ParameterEvent>(down)
                                                : std::span<const vloom::ParameterEvent>();
            context.sidechain = block % 2 == 0 ? key.data() : nullptr;
            context.transport.tempo = block % 4 < 2 ? 120.0 : 87.5; // tempo changes
            context.notes = block % 5 == 0 ? std::span<const vloom::NoteEvent>(notes)
                                           : std::span<const vloom::NoteEvent>();
            const vloom::test::AllocationCounter counter;
            processor->process(context); // block splits, values taken from the host, glides
            (void)processor->latencyFrames();
            (void)processor->gainReductionDb();
            for (int readout = 0; readout < readoutCount; ++readout)
                (void)processor->readoutValue(readout); // a UI timer may poll these meanwhile
            (void)processor->sampleSlotsVersion();
            if (block % 20 == 19)
                processor->reset();
            allocations += counter.count();
        }
        CHECK(allocations == 0);
        for (const float value : left)
            REQUIRE(std::isfinite(value));
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

TEST_CASE("Notes played by hand reach an instrument with its next block", "[plugin][builtin]")
{
    vloom::PluginInstance synth(vloom::createBuiltinProcessor("vloom.synth"));
    REQUIRE(synth.activate(kRate, 512));
    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    const auto block = [&] {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        const vloom::test::AllocationCounter counter;
        synth.process(left.data(), right.data(), 512, vloom::TransportInfo{});
        CHECK(counter.count() == 0);
        float most = 0.0f;
        for (float sample : left)
            most = std::max(most, std::abs(sample));
        return most;
    };
    CHECK(block() == 0.0f);
    REQUIRE(synth.playNote(60, 0.9f));
    CHECK(block() > 0.01f);
    CHECK(block() > 0.01f); // it holds
    REQUIRE(synth.stopNote(60));
    float tail = 1.0f;
    for (int i = 0; i < 200; ++i)
        tail = block();
    CHECK(tail == 0.0f);

    // Bypassed: played notes are dropped, not kept for when it is back.
    synth.setBypassed(true);
    REQUIRE(synth.playNote(64, 1.0f));
    (void)block();
    synth.setBypassed(false);
    CHECK(block() == 0.0f);

    // More than the queue holds are refused, and none of it breaks.
    std::size_t taken = 0;
    for (int i = 0; i < 200; ++i)
        taken += synth.playNote(i % 128, 1.0f) ? 1 : 0;
    CHECK(taken == vloom::PluginInstance::kMaxLiveNotes);
    CHECK_FALSE(synth.playNote(60, std::numeric_limits<float>::quiet_NaN()));
    CHECK(block() > 0.0f);
}

TEST_CASE("The built-in synth is an instrument that plays notes at their frames", "[plugin][builtin]")
{
    auto synth = vloom::createBuiltinProcessor("vloom.synth");
    REQUIRE(synth != nullptr);
    CHECK(synth->info().kind == vloom::PluginInfo::Kind::Instrument);
    CHECK(synth->info().name == "Synth");
    CHECK_FALSE(synth->parameters().empty());
    REQUIRE(synth->activate(kRate, 512));

    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    std::array<float*, 2> channels{left.data(), right.data()};
    vloom::ProcessContext context;
    context.channels = channels.data();
    context.numChannels = 2;
    context.numFrames = 512;
    const auto loudest = [&left](std::size_t from, std::size_t to) {
        float most = 0.0f;
        for (std::size_t i = from; i < to; ++i)
            most = std::max(most, std::abs(left[i]));
        return most;
    };

    synth->process(context);
    CHECK(loudest(0, 512) == 0.0f); // no notes: silence

    // A note that starts at frame 200: nothing before it, sound after.
    const std::array<vloom::NoteEvent, 1> on{vloom::NoteEvent{200, vloom::NoteEvent::Type::On, 60, 1.0f}};
    context.notes = on;
    long long allocations = 0;
    {
        const vloom::test::AllocationCounter counter;
        synth->process(context);
        allocations += counter.count();
    }
    CHECK(loudest(0, 200) == 0.0f);
    CHECK(loudest(200, 512) > 0.01f);

    // Let go at frame 100: it rings out, then it is silent again.
    const std::array<vloom::NoteEvent, 1> off{vloom::NoteEvent{100, vloom::NoteEvent::Type::Off, 60, 0.0f}};
    context.notes = off;
    float tail = 1.0f;
    for (int block = 0; block < 200; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        const vloom::test::AllocationCounter counter;
        synth->process(context);
        allocations += counter.count();
        context.notes = {};
        tail = loudest(0, 512);
    }
    CHECK(tail == 0.0f);
    CHECK(allocations == 0);

    // An effect is given no notes: the gain ignores them.
    auto gain = activeGain();
    context.notes = on;
    std::fill(left.begin(), left.end(), 0.5f);
    gain->process(context);
    CHECK(left[300] == 0.5f);
}
