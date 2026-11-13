// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vloom {

/// Sample formats a WAV file can be written in.
enum class WavFormat : std::uint8_t {
    Float32, ///< 32-bit IEEE float (WAVE_FORMAT_IEEE_FLOAT, with a `fact` chunk): no clipping, no dither
    Pcm16, ///< 16-bit integer PCM (CD)
    Pcm24, ///< 24-bit integer PCM
};

/// Bytes one sample of `format` takes.
[[nodiscard]] int wavBytesPerSample(WavFormat format) noexcept;

/// The most channels a WAV frame can hold (its size is 16 bits).
inline constexpr int kMaxWavChannels = 65535 / 4;

/// The header of a WAV file of `frames` frames, little-endian on every
/// platform; empty when it cannot be encoded: no channels or more than
/// kMaxWavChannels, a sample rate <= 0 or one whose byte rate overflows the
/// header, or more data than a 4 GiB RIFF file can hold.
[[nodiscard]] std::vector<std::uint8_t> encodeWavHeader(
    std::uint64_t frames, int channels, int sampleRate, WavFormat format);

/// encodeWavHeader() for WavFormat::Float32.
[[nodiscard]] std::vector<std::uint8_t> encodeWavFloat32Header(
    std::uint64_t frames, int channels, int sampleRate);

/// Encodes interleaved samples as a 32-bit IEEE float WAV file. Returns an
/// empty vector when they cannot be encoded (see encodeWavHeader(); also a
/// sample count that is not a whole number of frames).
[[nodiscard]] std::vector<std::uint8_t> encodeWavFloat32(
    std::span<const float> interleaved, int channels, int sampleRate);

/// Writes a WAV file piece by piece, so the whole signal never has to be in
/// memory. It goes to a temporary file next to `path` that replaces `path`
/// only in finish(): a failed or abandoned write never leaves a broken file
/// behind (nor clobbers an existing one).
///
/// Integer formats clamp to full scale; with `dither` they add triangular
/// (TPDF) noise of one least significant bit before rounding, which turns
/// quantisation distortion into a little steady noise. The noise is
/// deterministic: the same audio gives the same file.
class WavFileWriter {
public:
    struct Spec {
        int channels = 2;
        int sampleRate = 48000;
        WavFormat format = WavFormat::Float32;
        bool dither = true; ///< integer formats only
        /// The length, if known: finish() then checks every frame was written.
        /// Unknown: up to what a 4 GiB file holds; the header gets the real
        /// length in finish().
        std::optional<std::uint64_t> frames;
    };

    WavFileWriter() = default;
    WavFileWriter(const WavFileWriter&) = delete;
    WavFileWriter& operator=(const WavFileWriter&) = delete;
    /// Abandons an unfinished file: the temporary file is removed.
    ~WavFileWriter();

    /// Starts `path` (UTF-8). False with `error` when the format cannot be
    /// encoded or the file cannot be created.
    bool open(const std::string& path, const Spec& spec, std::string* error);
    /// open() for exactly `frames` frames of 32-bit float.
    bool open(
        const std::string& path, std::uint64_t frames, int channels, int sampleRate, std::string* error);
    /// Appends whole frames; more than the file can take is an error.
    bool write(std::span<const float> interleaved, std::string* error);
    /// Checks the length, writes the final header and puts the file in place.
    bool finish(std::string* error);

    [[nodiscard]] std::uint64_t framesWritten() const noexcept { return written_; }

private:
    void abandon() noexcept;
    void encode(std::span<const float> interleaved, std::vector<std::uint8_t>& out) noexcept;

    std::filesystem::path target_;
    std::filesystem::path temporary_;
    std::ofstream stream_;
    Spec spec_;
    std::uint64_t written_ = 0;
    std::uint64_t maxFrames_ = 0;
    std::uint32_t noise_ = 0x9E3779B9u; ///< dither generator state (xorshift32)
    bool open_ = false;
};

/// Writes encodeWavFloat32() to `path` (UTF-8), like WavFileWriter. On
/// failure returns false and fills `error`.
bool writeWavFloat32(const std::string& path, std::span<const float> interleaved, int channels,
    int sampleRate, std::string* error = nullptr);

} // namespace vloom
