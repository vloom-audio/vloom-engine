// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/render/WavWriter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::uint32_t u32At(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t u16At(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::string tagAt(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4)};
}

} // namespace

TEST_CASE("WAV header describes 32-bit float audio", "[render]")
{
    const std::vector<float> samples{0.0f, 0.5f, -0.5f, 1.0f, 0.25f, -1.0f}; // 3 stereo frames
    const auto wav = vloom::encodeWavFloat32(samples, 2, 48000);

    REQUIRE(wav.size() == 58 + samples.size() * 4);
    CHECK(tagAt(wav, 0) == "RIFF");
    CHECK(u32At(wav, 4) == wav.size() - 8);
    CHECK(tagAt(wav, 8) == "WAVE");

    CHECK(tagAt(wav, 12) == "fmt ");
    CHECK(u32At(wav, 16) == 18);
    CHECK(u16At(wav, 20) == 3); // WAVE_FORMAT_IEEE_FLOAT
    CHECK(u16At(wav, 22) == 2); // channels
    CHECK(u32At(wav, 24) == 48000); // sample rate
    CHECK(u32At(wav, 28) == 48000 * 8); // bytes per second
    CHECK(u16At(wav, 32) == 8); // block align
    CHECK(u16At(wav, 34) == 32); // bits per sample
    CHECK(u16At(wav, 36) == 0); // cbSize

    CHECK(tagAt(wav, 38) == "fact");
    CHECK(u32At(wav, 42) == 4);
    CHECK(u32At(wav, 46) == 3); // frames

    CHECK(tagAt(wav, 50) == "data");
    CHECK(u32At(wav, 54) == samples.size() * 4);
}

TEST_CASE("WAV samples are stored little-endian, bit for bit", "[render]")
{
    const std::vector<float> samples{0.1f, -0.75f, 3.0e-9f, 1.0f};
    const auto wav = vloom::encodeWavFloat32(samples, 1, 44100);
    REQUIRE(wav.size() == 58 + 16);
    for (std::size_t i = 0; i < samples.size(); ++i)
        CHECK(std::bit_cast<float>(u32At(wav, 58 + i * 4)) == samples[i]);
}

TEST_CASE("Unencodable input gives no WAV", "[render]")
{
    const std::vector<float> three{0.0f, 0.0f, 0.0f};
    CHECK(vloom::encodeWavFloat32(three, 2, 48000).empty()); // not whole frames
    CHECK(vloom::encodeWavFloat32(three, 0, 48000).empty());
    CHECK(vloom::encodeWavFloat32(three, 1, 0).empty());
    CHECK(vloom::encodeWavFloat32(three, 1, -1).empty());
    CHECK_FALSE(vloom::encodeWavFloat32({}, 1, 48000).empty()); // an empty file is valid
}

TEST_CASE("WAV files are written to UTF-8 paths", "[render]")
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path folder
        = std::filesystem::temp_directory_path() / ("vloom_wav_" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    // "klick-ä" in UTF-8: checks non-ASCII paths on Windows too.
    const std::string path = folder.string() + "/klick-\xC3\xA4.wav";

    const std::vector<float> samples{0.25f, -0.25f};
    std::string error;
    REQUIRE(vloom::writeWavFloat32(path, samples, 2, 48000, &error));
    CHECK(error.empty());

    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    const std::vector<std::uint8_t> written{
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(written == vloom::encodeWavFloat32(samples, 2, 48000));

    in.close();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

TEST_CASE("Writing to a missing folder reports an error", "[render]")
{
    std::string error;
    const std::vector<float> samples{0.0f};
    CHECK_FALSE(vloom::writeWavFloat32("/no/such/folder/x.wav", samples, 1, 48000, &error));
    CHECK_FALSE(error.empty());
}
