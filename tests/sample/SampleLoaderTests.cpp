// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/render/WavWriter.hpp>
#include <vloom/engine/sample/SampleLoader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

TEST_CASE("The size limits also hold after resampling", "[sample]")
{
    // Review finding: limits were checked on the file, not on the resampled
    // buffer that stays in memory.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path folder
        = std::filesystem::temp_directory_path() / ("vloom_loader_" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    const std::string path = (folder / "slow.wav").string();
    REQUIRE(vloom::writeWavFloat32(path, std::vector<float>(10000, 0.1f), 1, 1000)); // 10 s at 1 kHz

    vloom::WavReadLimits limits;
    limits.maxFrames = 100000; // the file has 10000; at 48 kHz it would be 480000
    std::string error;
    CHECK(vloom::loadSample(path, 48000.0, &error, limits) == nullptr);
    CHECK_FALSE(error.empty());
    CHECK(vloom::loadSample(path, 8000.0, &error, limits) != nullptr); // 80000 frames: fits

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}
