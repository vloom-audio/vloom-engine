// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sample/WavReader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <system_error>
#include <vector>

namespace vloom {

namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
constexpr int kMinSampleRate = 1000; ///< lower is no audio sample, and would explode when resampled
constexpr int kMaxSampleRate = 768000;
/// Float samples are clamped here (+60 dBFS): kept loud, far from overflow when mixed.
constexpr double kMaxFloatSample = 1024.0;

// Bytes 2..15 of every KSDATAFORMAT_SUBTYPE_* GUID; bytes 0..1 hold the format tag.
constexpr std::array<std::uint8_t, 14> kSubFormatTail{
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

/// Little-endian reads from a span. Callers check the range first.
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8)
        | (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint64_t u64(std::span<const std::uint8_t> b, std::size_t at)
{
    return static_cast<std::uint64_t>(u32(b, at)) | (static_cast<std::uint64_t>(u32(b, at + 4)) << 32);
}

bool tagIs(std::span<const std::uint8_t> b, std::size_t at, const char (&tag)[5])
{
    return std::memcmp(b.data() + at, tag, 4) == 0;
}

WavReadResult fail(std::string message)
{
    WavReadResult result;
    result.error = std::move(message);
    return result;
}

struct Format {
    std::uint16_t tag = 0; ///< kFormatPcm or kFormatFloat after resolving EXTENSIBLE
    int channels = 0;
    int sampleRate = 0;
    int bits = 0; ///< container size per sample
    std::uint16_t blockAlign = 0;
};

/// Reads the fmt chunk at [at, at + size). Returns an error message or "".
std::string parseFormat(std::span<const std::uint8_t> b, std::size_t at, std::uint64_t size,
    const WavReadLimits& limits, Format& format)
{
    if (size < 16)
        return "The WAV format chunk is too short";
    std::uint16_t tag = u16(b, at);
    format.channels = u16(b, at + 2);
    const std::uint32_t rate = u32(b, at + 4);
    format.blockAlign = u16(b, at + 12);
    format.bits = u16(b, at + 14);

    if (tag == kFormatExtensible) {
        if (size < 40 || u16(b, at + 16) < 22)
            return "The WAV extensible format chunk is too short";
        if (std::memcmp(b.data() + at + 26, kSubFormatTail.data(), kSubFormatTail.size()) != 0)
            return "Unsupported WAV sub-format";
        tag = u16(b, at + 24);
    }
    format.tag = tag;

    if (tag != kFormatPcm && tag != kFormatFloat)
        return "Unsupported WAV encoding (only PCM and float are read)";
    if (format.channels < 1 || format.channels > limits.maxChannels)
        return "Unsupported number of channels: " + std::to_string(format.channels);
    if (rate < static_cast<std::uint32_t>(kMinSampleRate)
        || rate > static_cast<std::uint32_t>(kMaxSampleRate))
        return "Unsupported sample rate: " + std::to_string(rate);
    format.sampleRate = static_cast<int>(rate);

    const bool pcmBits = format.bits == 8 || format.bits == 16 || format.bits == 24 || format.bits == 32;
    const bool floatBits = format.bits == 32 || format.bits == 64;
    if ((tag == kFormatPcm && !pcmBits) || (tag == kFormatFloat && !floatBits))
        return "Unsupported bit depth: " + std::to_string(format.bits);
    if (format.blockAlign != format.channels * (format.bits / 8))
        return "The WAV block size does not match its format";
    return {};
}

float decodeSample(std::span<const std::uint8_t> b, std::size_t at, const Format& format)
{
    if (format.tag == kFormatFloat) {
        const double value = format.bits == 32 ? static_cast<double>(std::bit_cast<float>(u32(b, at)))
                                               : std::bit_cast<double>(u64(b, at));
        // Non-finite samples would poison every mix they reach; huge finite
        // ones would overflow float (cast) or the mix (sums).
        if (!std::isfinite(value))
            return 0.0f;
        return static_cast<float>(std::clamp(value, -kMaxFloatSample, kMaxFloatSample));
    }
    switch (format.bits) {
    case 8:
        return static_cast<float>(static_cast<int>(b[at]) - 128) / 128.0f;
    case 16:
        return static_cast<float>(static_cast<std::int16_t>(u16(b, at))) / 32768.0f;
    case 24: {
        // Sign-extend the 24-bit value through the top of a 32-bit word.
        const auto raw = static_cast<std::uint32_t>(b[at]) << 8 | static_cast<std::uint32_t>(b[at + 1]) << 16
            | static_cast<std::uint32_t>(b[at + 2]) << 24;
        return static_cast<float>(static_cast<std::int32_t>(raw) >> 8) / 8388608.0f;
    }
    default: // 32
        return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(u32(b, at))) / 2147483648.0);
    }
}

} // namespace

WavReadResult decodeWav(std::span<const std::uint8_t> bytes, const WavReadLimits& limits)
{
    if (bytes.size() > limits.maxFileBytes)
        return fail("The WAV file is too large");
    if (bytes.size() < 12 || !tagIs(bytes, 0, "RIFF") || !tagIs(bytes, 8, "WAVE"))
        return fail("Not a WAV file");

    Format format;
    bool haveFormat = false;
    std::size_t dataAt = 0;
    std::uint64_t dataBytes = 0;
    bool haveData = false;

    // Walk the chunks. The RIFF size field is ignored: the real end is the
    // end of the buffer, so a wrong size can never lead outside it.
    const std::uint64_t end = bytes.size();
    std::uint64_t at = 12;
    while (at + 8 <= end) {
        const auto chunk = static_cast<std::size_t>(at);
        const std::uint64_t size = u32(bytes, chunk + 4);
        const std::uint64_t body = at + 8;
        const std::uint64_t available = end - body;

        if (tagIs(bytes, chunk, "fmt ")) {
            if (haveFormat)
                return fail("The WAV file has two format chunks");
            if (size > available)
                return fail("The WAV format chunk is cut off");
            if (const std::string error
                = parseFormat(bytes, static_cast<std::size_t>(body), size, limits, format);
                !error.empty())
                return fail(error);
            haveFormat = true;
        } else if (tagIs(bytes, chunk, "data")) {
            if (!haveData) {
                haveData = true;
                dataAt = static_cast<std::size_t>(body);
                // Unfinished recordings declare more (or 0xFFFFFFFF): read what is there.
                dataBytes = std::min(size, available);
            }
            if (size > available)
                break; // nothing valid can follow a cut-off data chunk
        }
        // Every other chunk (LIST, bext, cue, ...) is skipped unread.
        at = body + size + (size & 1); // chunks are padded to even sizes
    }

    if (!haveFormat)
        return fail("The WAV file has no format chunk");
    if (!haveData)
        return fail("The WAV file has no audio data");

    const std::int64_t frames = static_cast<std::int64_t>(dataBytes / format.blockAlign);
    if (frames > limits.maxFrames
        || static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(format.channels) * sizeof(float)
            > limits.maxDecodedBytes)
        return fail("The WAV file is too long");

    WavReadResult result;
    SampleBuffer buffer(format.channels, frames, format.sampleRate);
    const std::size_t bytesPerSample = static_cast<std::size_t>(format.bits / 8);
    for (int ch = 0; ch < format.channels; ++ch) {
        float* out = buffer.channel(ch);
        std::size_t at2 = dataAt + static_cast<std::size_t>(ch) * bytesPerSample;
        for (std::int64_t i = 0; i < frames; ++i, at2 += format.blockAlign)
            out[i] = decodeSample(bytes, at2, format);
    }
    result.buffer = std::move(buffer);
    return result;
}

WavReadResult readWavFile(const std::string& path, const WavReadLimits& limits)
{
    std::filesystem::path file;
    try {
        file = std::filesystem::path(std::u8string(path.begin(), path.end()));
    } catch (const std::exception&) { // not valid UTF-8
        return fail("'" + path + "' is not a valid file name");
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec)
        return fail("Could not open '" + path + "'");
    if (size > limits.maxFileBytes)
        return fail("The WAV file is too large");

    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return fail("Could not open '" + path + "'");
    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return fail("Not enough memory to read '" + path + "'");
    }
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        return fail("Could not read '" + path + "'");
    return decodeWav(bytes, limits);
}

} // namespace vloom
