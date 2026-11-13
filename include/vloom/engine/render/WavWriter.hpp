// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace vloom {

/// Encodes interleaved samples as a 32-bit IEEE float WAV file
/// (WAVE_FORMAT_IEEE_FLOAT, with the `fact` chunk non-PCM data needs),
/// little-endian on every platform.
///
/// Returns an empty vector when the input cannot be encoded: no channels or
/// more than kMaxWavChannels, a sample rate <= 0 or one whose byte rate
/// overflows the header, a sample count that is not a whole number of
/// frames, or more data than a 4 GiB RIFF file can hold.
[[nodiscard]] std::vector<std::uint8_t> encodeWavFloat32(
    std::span<const float> interleaved, int channels, int sampleRate);

/// The header encodeWavFloat32() starts with, for `frames` frames; empty
/// when they cannot be encoded (see there).
[[nodiscard]] std::vector<std::uint8_t> encodeWavFloat32Header(
    std::uint64_t frames, int channels, int sampleRate);

/// The most channels a 32-bit WAV frame can hold (its size is 16 bits).
inline constexpr int kMaxWavChannels = 65535 / 4;

/// Writes a 32-bit float WAV file of a known length piece by piece, so the
/// whole signal never has to be in memory. It goes to a temporary file next
/// to `path`, which replaces `path` only in finish(): a failed or abandoned
/// write never leaves a broken file behind (nor clobbers an existing one).
class WavFileWriter {
public:
    WavFileWriter() = default;
    WavFileWriter(const WavFileWriter&) = delete;
    WavFileWriter& operator=(const WavFileWriter&) = delete;
    /// Abandons an unfinished file: the temporary file is removed.
    ~WavFileWriter();

    /// Starts `path` (UTF-8) for `frames` frames. False with `error` when the
    /// format cannot be encoded or the file cannot be created.
    bool open(
        const std::string& path, std::uint64_t frames, int channels, int sampleRate, std::string* error);
    /// Appends whole frames; more than open() announced is an error.
    bool write(std::span<const float> interleaved, std::string* error);
    /// Checks that every frame was written and puts the file in place.
    bool finish(std::string* error);

private:
    void abandon() noexcept;

    std::filesystem::path target_;
    std::filesystem::path temporary_;
    std::ofstream stream_;
    std::uint64_t framesLeft_ = 0;
    int channels_ = 0;
    bool open_ = false;
};

/// Writes encodeWavFloat32() to `path` (UTF-8), like WavFileWriter. On
/// failure returns false and fills `error`.
bool writeWavFloat32(const std::string& path, std::span<const float> interleaved, int channels,
    int sampleRate, std::string* error = nullptr);

} // namespace vloom
