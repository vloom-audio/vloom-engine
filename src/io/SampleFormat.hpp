// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// Internal: the PCM sample formats a device buffer can hold, and the
// conversions between them and the engine's float samples. Pure code, built
// on every platform so it can be tested anywhere; WASAPI exclusive mode
// (ADR 0020) and ASIO (ADR 0021) use it.

#include <array>
#include <cstddef>
#include <cstdint>

namespace vloom::io {

enum class SampleFormat : std::uint8_t {
    Float32, ///< IEEE float, 32 bits
    Int32, ///< signed integer, 32 valid bits in 32
    Int24In32, ///< signed integer, 24 valid bits in a 32-bit container (most significant bits)
    Int24Packed, ///< signed integer, 24 bits in 3 bytes, little-endian
    Int16, ///< signed integer, 16 bits
    Float64, ///< IEEE float, 64 bits (ASIO Float64LSB)
    // N valid bits in the least significant bits of a 32-bit container,
    // sign-extended (ASIO Int32LSB16, Int32LSB18, Int32LSB20, Int32LSB24).
    Int32Lsb16,
    Int32Lsb18,
    Int32Lsb20,
    Int32Lsb24,
};

/// One format to ask the device for: WAVEFORMATEXTENSIBLE's fields, without
/// the channel mask (which is the platform's).
struct PcmFormat {
    SampleFormat sample = SampleFormat::Float32;
    int sampleRate = 0;
    int channels = 0;
    int containerBits = 32; ///< wBitsPerSample
    int validBits = 32; ///< Samples.wValidBitsPerSample
    bool isFloat = true; ///< KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, else _PCM
    int blockAlign = 0; ///< bytes per frame
    int bytesPerSecond = 0;
};

/// Bytes one sample takes in the device buffer.
[[nodiscard]] int bytesPerSample(SampleFormat format) noexcept;

/// Short name for messages ("float32", "int24 (32-bit container)", ...).
[[nodiscard]] const char* sampleFormatName(SampleFormat format) noexcept;

inline constexpr std::array<SampleFormat, 5> kExclusiveFormatOrder{SampleFormat::Float32, SampleFormat::Int32,
    SampleFormat::Int24In32, SampleFormat::Int24Packed, SampleFormat::Int16};

/// The formats to try for an exclusive stream at `sampleRate` and `channels`,
/// best first: float32, int32, int24 in 32, packed int24, int16.
[[nodiscard]] std::array<PcmFormat, kExclusiveFormatOrder.size()> exclusiveFormatCandidates(
    int sampleRate, int channels) noexcept;

/// Writes `count` samples from `in` to `out` in `format`, native byte order
/// (little-endian on every supported platform; packed int24 is always
/// little-endian). Samples are clamped to [-1, 1] first, so overs hard-clip
/// and never wrap; NaN becomes 0. Integers round to nearest (halves away
/// from zero) and scale by 2^(bits-1) - 1, so -1 and +1 map to symmetric
/// codes. Real-time safe: no allocation. `out` needs no alignment.
void convertFromFloat(SampleFormat format, const float* in, std::byte* out, std::size_t count) noexcept;

/// The reverse: reads `count` samples in `format` from `in` into floats.
/// Integers divide by the same 2^(bits-1) - 1, so a code of up to 24 bits
/// read here and written back by convertFromFloat() is unchanged; the most
/// negative code reads slightly below -1
/// (-32768 -> -1.00003) and is not clamped. The LSB-aligned formats
/// sign-extend from their valid bits, ignoring the container's upper bits.
/// Float NaN reads as 0. Real-time safe: no allocation. `in` needs no
/// alignment.
void convertToFloat(SampleFormat format, const std::byte* in, float* out, std::size_t count) noexcept;

} // namespace vloom::io
