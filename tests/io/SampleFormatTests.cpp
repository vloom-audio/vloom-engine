// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "io/SampleFormat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// Pure conversions for WASAPI exclusive output (ADR 0020) and ASIO (ADR
// 0021). No device is opened here or anywhere in these tests.

using vloom::io::convertFromFloat;
using vloom::io::SampleFormat;

namespace {

template <typename T> std::vector<T> convert(SampleFormat format, const std::vector<float>& in)
{
    std::vector<std::byte> bytes(in.size() * sizeof(T));
    convertFromFloat(format, in.data(), bytes.data(), in.size());
    std::vector<T> out(in.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

std::vector<std::int32_t> convertPacked24(const std::vector<float>& in)
{
    std::vector<std::byte> bytes(in.size() * 3);
    convertFromFloat(SampleFormat::Int24Packed, in.data(), bytes.data(), in.size());
    std::vector<std::int32_t> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        auto bits = static_cast<std::uint32_t>(bytes[i * 3])
            | (static_cast<std::uint32_t>(bytes[i * 3 + 1]) << 8U)
            | (static_cast<std::uint32_t>(bytes[i * 3 + 2]) << 16U);
        if ((bits & 0x800000U) != 0)
            bits |= 0xFF000000U; // sign-extend
        out.push_back(static_cast<std::int32_t>(bits));
    }
    return out;
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

} // namespace

TEST_CASE("int16 clips overs, rounds to nearest and maps NaN to 0", "[io]")
{
    const auto out = convert<std::int16_t>(SampleFormat::Int16,
        {0.0f, 1.0f, -1.0f, 1.5f, -7.0f, kNaN, kInf, -kInf, 0.5f, -0.5f, 1.0f / 32767.0f, 0.4f / 32767.0f,
            0.6f / 32767.0f, -0.6f / 32767.0f});
    CHECK(out
        == std::vector<std::int16_t>{
            0, 32767, -32767, 32767, -32767, 0, 32767, -32767, 16384, -16384, 1, 0, 1, -1});
}

TEST_CASE("int24 in a 32-bit container is left-justified", "[io]")
{
    const auto out = convert<std::int32_t>(
        SampleFormat::Int24In32, {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, kNaN, 1.0f / 8388607.0f});
    CHECK(out
        == std::vector<std::int32_t>{
            0, 8388607 * 256, -8388607 * 256, 8388607 * 256, -8388607 * 256, 0, 256});
    for (const auto value : out)
        CHECK((value & 0xFF) == 0); // low byte unused
}

TEST_CASE("Packed int24 takes 3 little-endian bytes per sample", "[io]")
{
    CHECK(convertPacked24({0.0f, 1.0f, -1.0f, 3.0f, -3.0f, kNaN, -1.0f / 8388607.0f})
        == std::vector<std::int32_t>{0, 8388607, -8388607, 8388607, -8388607, 0, -1});

    const std::vector<float> in{1.0f, -1.0f / 8388607.0f};
    std::vector<std::byte> bytes(6);
    convertFromFloat(SampleFormat::Int24Packed, in.data(), bytes.data(), in.size());
    CHECK(bytes[0] == std::byte{0xFF});
    CHECK(bytes[1] == std::byte{0xFF});
    CHECK(bytes[2] == std::byte{0x7F});
    CHECK(bytes[3] == std::byte{0xFF});
    CHECK(bytes[4] == std::byte{0xFF});
    CHECK(bytes[5] == std::byte{0xFF});
}

TEST_CASE("int32 reaches its extremes without overflow", "[io]")
{
    const auto out = convert<std::int32_t>(
        SampleFormat::Int32, {1.0f, -1.0f, 100.0f, -100.0f, kInf, -kInf, kNaN, 0.5f, 0.99999994f});
    CHECK(out[0] == 2147483647);
    CHECK(out[1] == -2147483647);
    CHECK(out[2] == 2147483647);
    CHECK(out[3] == -2147483647);
    CHECK(out[4] == 2147483647);
    CHECK(out[5] == -2147483647);
    CHECK(out[6] == 0);
    CHECK(out[7] == 1073741824); // 0.5 * (2^31 - 1) = 1073741823.5, rounded away from zero
    // The largest float below 1 stays below full scale (computed in double).
    CHECK(out[8] == 2147483519);
}

TEST_CASE("float32 passes in-range samples bit for bit and clips the rest", "[io]")
{
    const std::vector<float> in{
        0.0f, -0.0f, 0.123456789f, -0.999999f, 1.0f, -1.0f, 1.0001f, -4.0f, kNaN, kInf};
    const auto out = convert<float>(SampleFormat::Float32, in);
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(std::memcmp(&out[i], &in[i], sizeof(float)) == 0);
    CHECK(out[6] == 1.0f);
    CHECK(out[7] == -1.0f);
    CHECK(out[8] == 0.0f);
    CHECK(out[9] == 1.0f);
}

TEST_CASE("Converting zero samples writes nothing", "[io]")
{
    std::byte sentinel{0xAB};
    convertFromFloat(SampleFormat::Int16, nullptr, &sentinel, 0);
    CHECK(sentinel == std::byte{0xAB});
}

TEST_CASE("Exclusive format candidates come best first with consistent sizes", "[io]")
{
    const auto candidates = vloom::io::exclusiveFormatCandidates(44100, 2);
    REQUIRE(candidates.size() == 5);

    struct Expected {
        SampleFormat sample;
        int containerBits;
        int validBits;
        bool isFloat;
    };
    const Expected expected[] = {
        {SampleFormat::Float32, 32, 32, true},
        {SampleFormat::Int32, 32, 32, false},
        {SampleFormat::Int24In32, 32, 24, false},
        {SampleFormat::Int24Packed, 24, 24, false},
        {SampleFormat::Int16, 16, 16, false},
    };
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        CHECK(candidate.sample == expected[i].sample);
        CHECK(candidate.containerBits == expected[i].containerBits);
        CHECK(candidate.validBits == expected[i].validBits);
        CHECK(candidate.isFloat == expected[i].isFloat);
        CHECK(candidate.sampleRate == 44100);
        CHECK(candidate.channels == 2);
        CHECK(candidate.blockAlign == 2 * candidate.containerBits / 8);
        CHECK(candidate.blockAlign == 2 * vloom::io::bytesPerSample(candidate.sample));
        CHECK(candidate.bytesPerSecond == 44100 * candidate.blockAlign);
        CHECK(std::strlen(vloom::io::sampleFormatName(candidate.sample)) > 0);
    }

    const auto surround = vloom::io::exclusiveFormatCandidates(96000, 6);
    CHECK(surround[3].blockAlign == 18); // packed int24, 6 channels
    CHECK(surround[3].bytesPerSecond == 96000 * 18);
}

// ---- the formats ASIO adds (ADR 0021), and reading samples back

namespace {

template <typename T> std::vector<float> read(SampleFormat format, const std::vector<T>& in)
{
    std::vector<std::byte> bytes(in.size() * sizeof(T));
    std::memcpy(bytes.data(), in.data(), bytes.size());
    std::vector<float> out(in.size(), 99.0f);
    vloom::io::convertToFloat(format, bytes.data(), out.data(), in.size());
    return out;
}

const SampleFormat kAllFormats[] = {SampleFormat::Float32, SampleFormat::Int32, SampleFormat::Int24In32,
    SampleFormat::Int24Packed, SampleFormat::Int16, SampleFormat::Float64, SampleFormat::Int32Lsb16,
    SampleFormat::Int32Lsb18, SampleFormat::Int32Lsb20, SampleFormat::Int32Lsb24};

} // namespace

TEST_CASE("float64 clips like float32 and keeps in-range samples exact", "[io]")
{
    const auto out
        = convert<double>(SampleFormat::Float64, {0.0f, 0.25f, -0.123456789f, 1.5f, -3.0f, kNaN, kInf});
    CHECK(out[0] == 0.0);
    CHECK(out[1] == 0.25);
    CHECK(out[2] == static_cast<double>(-0.123456789f));
    CHECK(out[3] == 1.0);
    CHECK(out[4] == -1.0);
    CHECK(out[5] == 0.0);
    CHECK(out[6] == 1.0);
    CHECK(vloom::io::bytesPerSample(SampleFormat::Float64) == 8);
}

TEST_CASE("LSB-aligned int formats are right-justified and sign-extended", "[io]")
{
    CHECK(convert<std::int32_t>(SampleFormat::Int32Lsb16, {1.0f, -1.0f, 2.0f, kNaN, 0.5f})
        == std::vector<std::int32_t>{32767, -32767, 32767, 0, 16384});
    CHECK(convert<std::int32_t>(SampleFormat::Int32Lsb18, {1.0f, -1.0f, -9.0f})
        == std::vector<std::int32_t>{131071, -131071, -131071});
    CHECK(convert<std::int32_t>(SampleFormat::Int32Lsb20, {1.0f, -1.0f, 1.0f / 524287.0f})
        == std::vector<std::int32_t>{524287, -524287, 1});
    CHECK(convert<std::int32_t>(SampleFormat::Int32Lsb24, {1.0f, -1.0f, -1.0f / 8388607.0f})
        == std::vector<std::int32_t>{8388607, -8388607, -1});
    for (const SampleFormat format : {SampleFormat::Int32Lsb16, SampleFormat::Int32Lsb18,
             SampleFormat::Int32Lsb20, SampleFormat::Int32Lsb24})
        CHECK(vloom::io::bytesPerSample(format) == 4);
}

TEST_CASE("Integer samples read back as float by the same scale", "[io]")
{
    CHECK(read<std::int16_t>(SampleFormat::Int16, {0, 32767, -32767, -32768, 16384})
        == std::vector<float>{0.0f, 1.0f, -1.0f, static_cast<float>(-32768.0 / 32767.0),
            static_cast<float>(16384.0 / 32767.0)});
    CHECK(read<std::int32_t>(SampleFormat::Int32, {0, 2147483647, -2147483647})
        == std::vector<float>{0.0f, 1.0f, -1.0f});
    CHECK(read<std::int32_t>(SampleFormat::Int24In32, {8388607 * 256, -8388607 * 256, 256 + 0x7F})
        == std::vector<float>{1.0f, -1.0f, static_cast<float>(1.0 / 8388607.0)});
    CHECK(read<std::int32_t>(SampleFormat::Int32Lsb24, {8388607, -8388607, -1})
        == std::vector<float>{1.0f, -1.0f, static_cast<float>(-1.0 / 8388607.0)});
    CHECK(read<std::int32_t>(SampleFormat::Int32Lsb20, {524287, -524287}) == std::vector<float>{1.0f, -1.0f});
    CHECK(read<std::int32_t>(SampleFormat::Int32Lsb18, {131071, -131071}) == std::vector<float>{1.0f, -1.0f});
    CHECK(read<std::int32_t>(SampleFormat::Int32Lsb16, {32767, -32767}) == std::vector<float>{1.0f, -1.0f});
    // Bits above the valid ones are ignored: the low 16 bits are 0x8001 = -32767.
    const auto stray = static_cast<std::int32_t>(0x12348001U);
    CHECK(read<std::int32_t>(SampleFormat::Int32Lsb16, {stray}) == std::vector<float>{-1.0f});

    const std::vector<std::byte> packed{std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    std::vector<float> out(3);
    vloom::io::convertToFloat(SampleFormat::Int24Packed, packed.data(), out.data(), 3);
    CHECK(out
        == std::vector<float>{
            1.0f, static_cast<float>(-8388607.0 / 8388607.0), static_cast<float>(-1.0 / 8388607.0)});
}

TEST_CASE("Float samples read back unchanged, non-finite as 0, overs kept", "[io]")
{
    CHECK(read<float>(SampleFormat::Float32, {0.5f, -1.5f, kNaN, kInf, -kInf})
        == std::vector<float>{0.5f, -1.5f, 0.0f, 0.0f, 0.0f});
    constexpr double kInf64 = std::numeric_limits<double>::infinity();
    constexpr float kMax = std::numeric_limits<float>::max();
    CHECK(read<double>(SampleFormat::Float64,
              {0.5, -2.0, std::numeric_limits<double>::quiet_NaN(), kInf64, -kInf64, 1e300, -1e300})
        == std::vector<float>{0.5f, -2.0f, 0.0f, 0.0f, 0.0f, kMax, -kMax});
}

TEST_CASE("Every format round-trips codes of up to 24 bits", "[io]")
{
    // Samples on the format's own grid survive a write and a read.
    for (const SampleFormat format : kAllFormats) {
        INFO(vloom::io::sampleFormatName(format));
        CHECK(std::strlen(vloom::io::sampleFormatName(format)) > 0);
        const std::vector<float> in{0.0f, 1.0f, -1.0f, 0.5f, -0.25f};
        std::vector<std::byte> bytes(in.size() * static_cast<std::size_t>(vloom::io::bytesPerSample(format)));
        vloom::io::convertFromFloat(format, in.data(), bytes.data(), in.size());
        std::vector<float> back(in.size());
        vloom::io::convertToFloat(format, bytes.data(), back.data(), in.size());
        std::vector<std::byte> again(bytes.size());
        vloom::io::convertFromFloat(format, back.data(), again.data(), back.size());
        CHECK(again == bytes);
        for (std::size_t i = 0; i < in.size(); ++i)
            CHECK(std::abs(back[i] - in[i]) <= 1.0f / 32767.0f);
    }
}
