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
constexpr double kScale18 = 131071.0;
constexpr double kScale20 = 524287.0;
constexpr double kScale24 = 8388607.0;
constexpr double kScale32 = 2147483647.0;

/// Integer code to float, by the same scale as toInteger().
float fromInteger(std::int64_t code, double scale) noexcept
{
    return static_cast<float>(static_cast<double>(code) / scale);
}

/// The low `bits` bits of a 32-bit container, sign-extended.
std::int32_t lowBits(std::uint32_t container, unsigned bits) noexcept
{
    const unsigned shift = 32U - bits;
    return static_cast<std::int32_t>(container << shift) >> shift;
}

std::uint32_t readU32(const std::byte* in) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, in, 4);
    return value;
}

void writeInt32(std::byte* out, std::int64_t code) noexcept
{
    const auto value = static_cast<std::int32_t>(code);
    std::memcpy(out, &value, 4);
}

} // namespace

int bytesPerSample(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::Int24Packed:
        return 3;
    case SampleFormat::Int16:
        return 2;
    case SampleFormat::Float64:
        return 8;
    case SampleFormat::Float32:
    case SampleFormat::Int32:
    case SampleFormat::Int24In32:
    case SampleFormat::Int32Lsb16:
    case SampleFormat::Int32Lsb18:
    case SampleFormat::Int32Lsb20:
    case SampleFormat::Int32Lsb24:
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
    case SampleFormat::Float64:
        return "float64";
    case SampleFormat::Int32Lsb16:
        return "int16 (low bits of 32)";
    case SampleFormat::Int32Lsb18:
        return "int18 (low bits of 32)";
    case SampleFormat::Int32Lsb20:
        return "int20 (low bits of 32)";
    case SampleFormat::Int32Lsb24:
        return "int24 (low bits of 32)";
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
    case SampleFormat::Float64:
        for (std::size_t i = 0; i < count; ++i) {
            const double value = clampSample(in[i]);
            std::memcpy(out + i * 8, &value, 8);
        }
        return;
    case SampleFormat::Int32Lsb16:
        for (std::size_t i = 0; i < count; ++i)
            writeInt32(out + i * 4, toInteger(in[i], kScale16));
        return;
    case SampleFormat::Int32Lsb18:
        for (std::size_t i = 0; i < count; ++i)
            writeInt32(out + i * 4, toInteger(in[i], kScale18));
        return;
    case SampleFormat::Int32Lsb20:
        for (std::size_t i = 0; i < count; ++i)
            writeInt32(out + i * 4, toInteger(in[i], kScale20));
        return;
    case SampleFormat::Int32Lsb24:
        for (std::size_t i = 0; i < count; ++i)
            writeInt32(out + i * 4, toInteger(in[i], kScale24));
        return;
    }
}

void convertToFloat(SampleFormat format, const std::byte* in, float* out, std::size_t count) noexcept
{
    switch (format) {
    case SampleFormat::Float32:
        for (std::size_t i = 0; i < count; ++i) {
            float value = 0.0f;
            std::memcpy(&value, in + i * 4, 4);
            out[i] = value == value ? value : 0.0f; // NaN as 0
        }
        return;
    case SampleFormat::Float64:
        for (std::size_t i = 0; i < count; ++i) {
            double value = 0.0;
            std::memcpy(&value, in + i * 8, 8);
            out[i] = value == value ? static_cast<float>(value) : 0.0f;
        }
        return;
    case SampleFormat::Int32:
        for (std::size_t i = 0; i < count; ++i)
            out[i] = fromInteger(static_cast<std::int32_t>(readU32(in + i * 4)), kScale32);
        return;
    case SampleFormat::Int24In32:
        for (std::size_t i = 0; i < count; ++i)
            out[i] = fromInteger(static_cast<std::int32_t>(readU32(in + i * 4)) >> 8, kScale24);
        return;
    case SampleFormat::Int24Packed:
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t bits = static_cast<std::uint32_t>(in[i * 3])
                | (static_cast<std::uint32_t>(in[i * 3 + 1]) << 8U)
                | (static_cast<std::uint32_t>(in[i * 3 + 2]) << 16U);
            out[i] = fromInteger(lowBits(bits, 24U), kScale24);
        }
        return;
    case SampleFormat::Int16:
        for (std::size_t i = 0; i < count; ++i) {
            std::int16_t value = 0;
            std::memcpy(&value, in + i * 2, 2);
            out[i] = fromInteger(value, kScale16);
        }
        return;
    case SampleFormat::Int32Lsb16:
        for (std::size_t i = 0; i < count; ++i)
            out[i] = fromInteger(lowBits(readU32(in + i * 4), 16U), kScale16);
        return;
    case SampleFormat::Int32Lsb18:
        for (std::size_t i = 0; i < count; ++i)
            out[i] = fromInteger(lowBits(readU32(in + i * 4), 18U), kScale18);
        return;
    case SampleFormat::Int32Lsb20:
        for (std::size_t i = 0; i < count; ++i)
            out[i] = fromInteger(lowBits(readU32(in + i * 4), 20U), kScale20);
        return;
    case SampleFormat::Int32Lsb24:
        for (std::size_t i = 0; i < count; ++i)
            out[i] = fromInteger(lowBits(readU32(in + i * 4), 24U), kScale24);
        return;
    }
}

} // namespace vloom::io
