// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vloom {

/// Encodes interleaved samples as a 32-bit IEEE float WAV file
/// (WAVE_FORMAT_IEEE_FLOAT, with the `fact` chunk non-PCM data needs),
/// little-endian on every platform.
///
/// Returns an empty vector when the input cannot be encoded: no channels, a
/// sample rate <= 0, a sample count that is not a whole number of frames, or
/// more data than a 4 GiB RIFF file can hold.
[[nodiscard]] std::vector<std::uint8_t> encodeWavFloat32(
    std::span<const float> interleaved, int channels, int sampleRate);

/// Writes encodeWavFloat32() to `path` (UTF-8). On failure returns false and
/// fills `error`.
bool writeWavFloat32(const std::string& path, std::span<const float> interleaved, int channels,
    int sampleRate, std::string* error = nullptr);

} // namespace vloom
