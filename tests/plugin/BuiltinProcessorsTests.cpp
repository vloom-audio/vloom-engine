// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/plugin/BuiltinProcessors.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
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
