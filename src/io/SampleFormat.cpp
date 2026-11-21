// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "io/SampleFormat.hpp"

#include <cstring>

namespace vloom::io {

namespace {

/// Clamped to [-1, 1], NaN as 0, in double so that the int32 scale stays exact.
double clampSample(float sample) noexcept
{
    const double value = static_cast<double>(sample);
    if (!(value == value)) // NaN
        return 0.0;
    if (value > 1.0)
        return 1.0;
    if (value < -1.0)
        return -1.0;
    return value;
}

/// Rounds the clamped sample times `scale` to nearest, halves away from zero.
/// The result lies in [-scale, scale].
std::int64_t toInteger(float sample, double scale) noexcept
{
    const double scaled = clampSample(sample) * scale;
    return static_cast<std::int64_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
}

constexpr double kScale16 = 32767.0;
constexpr double kScale24 = 8388607.0;
constexpr double kScale32 = 2147483647.0;

} // namespace

int bytesPerSample(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::Int24Packed:
        return 3;
    case SampleFormat::Int16:
        return 2;
    case SampleFormat::Float32:
    case SampleFormat::Int32:
    case SampleFormat::Int24In32:
        break;
    }
    return 4;
}

const char* sampleFormatName(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::Float32:
        return "float32";
    case SampleFormat::Int32:
        return "int32";
    case SampleFormat::Int24In32:
        return "int24 (32-bit container)";
    case SampleFormat::Int24Packed:
        return "int24 (packed)";
    case SampleFormat::Int16:
        return "int16";
    }
    return "unknown";
}

std::array<PcmFormat, kExclusiveFormatOrder.size()> exclusiveFormatCandidates(
    int sampleRate, int channels) noexcept
{
    std::array<PcmFormat, kExclusiveFormatOrder.size()> result{};
    for (std::size_t i = 0; i < kExclusiveFormatOrder.size(); ++i) {
        PcmFormat& format = result[i];
        format.sample = kExclusiveFormatOrder[i];
        format.sampleRate = sampleRate;
        format.channels = channels;
        format.containerBits = bytesPerSample(format.sample) * 8;
        format.validBits = format.sample == SampleFormat::Int24In32 ? 24 : format.containerBits;
        format.isFloat = format.sample == SampleFormat::Float32;
        format.blockAlign = bytesPerSample(format.sample) * channels;
        format.bytesPerSecond = format.blockAlign * sampleRate;
    }
    return result;
}

void convertFromFloat(SampleFormat format, const float* in, std::byte* out, std::size_t count) noexcept
{
    switch (format) {
    case SampleFormat::Float32:
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = static_cast<float>(clampSample(in[i]));
            std::memcpy(out + i * 4, &value, 4);
        }
        return;
    case SampleFormat::Int32:
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = static_cast<std::int32_t>(toInteger(in[i], kScale32));
            std::memcpy(out + i * 4, &value, 4);
        }
        return;
    case SampleFormat::Int24In32:
        for (std::size_t i = 0; i < count; ++i) {
            // The 24 valid bits are the container's most significant ones.
            const auto bits
                = static_cast<std::uint32_t>(static_cast<std::int32_t>(toInteger(in[i], kScale24))) << 8U;
            const auto value = static_cast<std::int32_t>(bits);
            std::memcpy(out + i * 4, &value, 4);
        }
        return;
    case SampleFormat::Int24Packed:
        for (std::size_t i = 0; i < count; ++i) {
            const auto bits
                = static_cast<std::uint32_t>(static_cast<std::int32_t>(toInteger(in[i], kScale24)));
            out[i * 3] = static_cast<std::byte>(bits & 0xFFU);
            out[i * 3 + 1] = static_cast<std::byte>((bits >> 8U) & 0xFFU);
            out[i * 3 + 2] = static_cast<std::byte>((bits >> 16U) & 0xFFU);
        }
        return;
    case SampleFormat::Int16:
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = static_cast<std::int16_t>(toInteger(in[i], kScale16));
            std::memcpy(out + i * 2, &value, 2);
        }
        return;
    }
}

} // namespace vloom::io
