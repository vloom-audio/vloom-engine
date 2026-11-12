// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/io/AudioDevice.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// These run without sound hardware (CI): only the paths that do not need a
// device. The tests tagged [hardware] are hidden ([.]): run them on a machine
// with an output device with `vloom_engine_tests "[hardware]"`.

TEST_CASE("A closed device reports nothing and ignores stop and close", "[io]")
{
    vloom::AudioDevice device;
    CHECK_FALSE(device.isOpen());
    CHECK_FALSE(device.isRunning());
    CHECK(device.sampleRate() == 0);
    CHECK(device.channels() == 0);
    CHECK(device.deviceName().empty());
    device.stop();
    device.close();

    std::string error;
    CHECK_FALSE(device.start(&error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Opening needs a callback and a listed device", "[io]")
{
    vloom::AudioDevice device;
    std::string error;
    CHECK_FALSE(device.open({}, nullptr, &error));
    CHECK_FALSE(error.empty());

    vloom::AudioDeviceConfig config;
    config.deviceIndex = 100000;
    error.clear();
    CHECK_FALSE(device.open(config, [](float* const*, int, int) { }, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(device.isOpen());
}

TEST_CASE("Listing devices works without hardware", "[io]")
{
    vloom::AudioDevice device;
    const auto devices = device.listOutputDevices();
    int defaults = 0;
    for (const auto& info : devices) {
        CHECK_FALSE(info.name.empty());
        defaults += info.isDefault ? 1 : 0;
    }
    CHECK(defaults <= 1);
}

TEST_CASE("The default device plays and stops", "[.][hardware][io]")
{
    vloom::AudioDevice device;
    std::atomic<long long> frames{0};
    std::atomic<int> maxBlock{0};

    vloom::AudioDeviceConfig config;
    config.sampleRate = 48000;
    config.bufferFrames = 256;
    std::string error;
    REQUIRE(device.open(
        config,
        [&](float* const* out, int channels, int numFrames) {
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < numFrames; ++i)
                    out[ch][i] = 0.0f; // silence
            frames += numFrames;
            if (numFrames > maxBlock.load())
                maxBlock = numFrames;
        },
        &error));
    INFO(error);
    CHECK(device.isOpen());
    CHECK(device.sampleRate() == 48000);
    CHECK(device.channels() == 2);
    CHECK_FALSE(device.deviceName().empty());

    REQUIRE(device.start(&error));
    CHECK(device.isRunning());
    // Hardware test: waiting for the device is the point.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (frames.load() < 48000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    device.stop();
    CHECK_FALSE(device.isRunning());

    CHECK(frames.load() >= 48000); // at least one second of audio requested
    CHECK(maxBlock.load() <= vloom::AudioDevice::kMaxBlockFrames);

    // Stopped means no more callbacks.
    const long long after = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(frames.load() == after);

    // Restart after stop.
    REQUIRE(device.start(&error));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    device.close();
    CHECK(frames.load() > after);
    CHECK_FALSE(device.isOpen());
}

TEST_CASE("Every listed device opens", "[.][hardware][io]")
{
    vloom::AudioDevice device;
    const auto devices = device.listOutputDevices();
    REQUIRE_FALSE(devices.empty());
    for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
        vloom::AudioDeviceConfig config;
        config.deviceIndex = i;
        std::string error;
        INFO(devices[static_cast<std::size_t>(i)].name);
        CHECK(device.open(config, [](float* const*, int, int) { }, &error));
        device.close();
    }
}
