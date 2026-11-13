// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/WavWriter.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <system_error>

namespace vloom {

namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatIeeeFloat = 3;
constexpr std::uint64_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();

/// Size of the fmt chunk's body: WAVEFORMATEX with cbSize = 0 for float,
/// PCMWAVEFORMAT for integers.
std::uint32_t fmtChunkSize(WavFormat format) { return format == WavFormat::Float32 ? 18 : 16; }

/// Bytes counted by the RIFF size before the samples: "WAVE", the fmt chunk,
/// the fact chunk (float only: non-PCM data needs it), the data chunk header.
std::uint64_t headerBytesInRiff(WavFormat format)
{
    return 4 + (8 + fmtChunkSize(format)) + (format == WavFormat::Float32 ? 8 + 4 : 0) + 8;
}

void put(std::vector<std::uint8_t>& out, const char (&tag)[5]) { out.insert(out.end(), tag, tag + 4); }

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

bool fail(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}

/// The most frames a 4 GiB file of this layout holds.
std::uint64_t maxFrames(int channels, WavFormat format)
{
    const auto blockAlign
        = static_cast<std::uint64_t>(channels) * static_cast<std::uint64_t>(wavBytesPerSample(format));
    return (kMaxU32 - headerBytesInRiff(format)) / blockAlign;
}

} // namespace

int wavBytesPerSample(WavFormat format) noexcept
{
    switch (format) {
    case WavFormat::Pcm16:
        return 2;
    case WavFormat::Pcm24:
        return 3;
    case WavFormat::Float32:
        break;
    }
    return 4;
}

std::vector<std::uint8_t> encodeWavHeader(
    std::uint64_t frames, int channels, int sampleRate, WavFormat format)
{
    if (channels < 1 || channels > kMaxWavChannels || sampleRate <= 0)
        return {};
    const int bytes = wavBytesPerSample(format);
    const auto blockAlign = static_cast<std::uint16_t>(channels * bytes);
    const std::uint64_t byteRate = static_cast<std::uint64_t>(sampleRate) * blockAlign;
    if (byteRate > kMaxU32 || frames > maxFrames(channels, format))
        return {}; // more than 4 GiB would need RF64
    const std::uint64_t dataBytes = frames * blockAlign;
    const bool isFloat = format == WavFormat::Float32;

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(8 + headerBytesInRiff(format)));
    put(out, "RIFF");
    putU32(out, static_cast<std::uint32_t>(headerBytesInRiff(format) + dataBytes));
    put(out, "WAVE");

    put(out, "fmt ");
    putU32(out, fmtChunkSize(format));
    putU16(out, isFloat ? kFormatIeeeFloat : kFormatPcm);
    putU16(out, static_cast<std::uint16_t>(channels));
    putU32(out, static_cast<std::uint32_t>(sampleRate));
    putU32(out, static_cast<std::uint32_t>(byteRate));
    putU16(out, blockAlign);
    putU16(out, static_cast<std::uint16_t>(bytes * 8)); // bits per sample
    if (isFloat) {
        putU16(out, 0); // cbSize
        put(out, "fact");
        putU32(out, 4);
        putU32(out, static_cast<std::uint32_t>(frames));
    }

    put(out, "data");
    putU32(out, static_cast<std::uint32_t>(dataBytes));
    return out;
}

std::vector<std::uint8_t> encodeWavFloat32Header(std::uint64_t frames, int channels, int sampleRate)
{
    return encodeWavHeader(frames, channels, sampleRate, WavFormat::Float32);
}

std::vector<std::uint8_t> encodeWavFloat32(std::span<const float> interleaved, int channels, int sampleRate)
{
    if (channels < 1 || interleaved.size() % static_cast<std::size_t>(channels) != 0)
        return {};
    std::vector<std::uint8_t> out = encodeWavFloat32Header(
        static_cast<std::uint64_t>(interleaved.size() / static_cast<std::size_t>(channels)), channels,
        sampleRate);
    if (out.empty())
        return {};
    out.reserve(out.size() + interleaved.size() * 4);
    for (const float sample : interleaved)
        putU32(out, std::bit_cast<std::uint32_t>(sample));
    return out;
}

WavFileWriter::~WavFileWriter() { abandon(); }

void WavFileWriter::abandon() noexcept
{
    if (!open_)
        return;
    open_ = false;
    stream_.close();
    std::error_code ec;
    std::filesystem::remove(temporary_, ec);
}

bool WavFileWriter::open(
    const std::string& path, std::uint64_t frames, int channels, int sampleRate, std::string* error)
{
    Spec spec;
    spec.channels = channels;
    spec.sampleRate = sampleRate;
    spec.frames = frames;
    return open(path, spec, error);
}

bool WavFileWriter::open(const std::string& path, const Spec& spec, std::string* error)
{
    abandon();
    const std::vector<std::uint8_t> header
        = encodeWavHeader(spec.frames.value_or(0), spec.channels, spec.sampleRate, spec.format);
    if (header.empty())
        return fail(error, "Cannot encode this audio as WAV (bad format, or larger than 4 GiB)");
    try {
        // UTF-8 path on every platform (Windows paths are wide). Converting
        // a path that is not valid UTF-8 throws.
        target_ = std::filesystem::path(std::u8string(path.begin(), path.end()));
    } catch (const std::exception&) {
        return fail(error, "'" + path + "' is not a valid file name");
    }

    // An existing file must be replaceable: find out now, not after a long
    // render (opening without truncating leaves it as it is).
    std::error_code ec;
    if (std::filesystem::exists(target_, ec)) {
        std::fstream probe(target_, std::ios::binary | std::ios::in | std::ios::out);
        if (!probe)
            return fail(error, "'" + path + "' is read-only or in use by another program");
    }

    // A name of its own, so two writers (or a user's file) never collide.
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp
        = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 16 && !stream_.is_open(); ++attempt) {
        temporary_ = target_;
        temporary_ += ".part-" + std::to_string(stamp ^ (counter.fetch_add(1) * 0x9E3779B97F4A7C15ULL));
        if (std::filesystem::exists(temporary_, ec))
            continue;
        stream_.open(temporary_, std::ios::binary | std::ios::trunc);
        if (!stream_)
            return fail(error, "Could not open '" + path + "' for writing");
    }
    if (!stream_.is_open())
        return fail(error, "Could not open '" + path + "' for writing");
    open_ = true;
    stream_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!stream_) {
        abandon();
        return fail(error, "Failed while writing '" + path + "'");
    }
    spec_ = spec;
    written_ = 0;
    maxFrames_ = spec.frames.value_or(maxFrames(spec.channels, spec.format));
    noise_ = 0x9E3779B9u;
    return true;
}

void WavFileWriter::encode(std::span<const float> interleaved, std::vector<std::uint8_t>& out) noexcept
{
    if (spec_.format == WavFormat::Float32) {
        for (const float sample : interleaved)
            putU32(out, std::bit_cast<std::uint32_t>(sample));
        return;
    }
    const bool is16 = spec_.format == WavFormat::Pcm16;
    const double scale = is16 ? 32768.0 : 8388608.0;
    const double top = scale - 1.0;
    // Uniform in [0, 1) from xorshift32: its top 24 bits.
    const auto uniform = [this] {
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        return static_cast<double>(noise_ >> 8) * (1.0 / 16777216.0);
    };
    for (const float sample : interleaved) {
        double value = std::isfinite(sample) ? static_cast<double>(sample) * scale : 0.0;
        if (spec_.dither)
            value += uniform() - uniform(); // triangular, +-1 LSB
        const auto quantised = static_cast<std::int32_t>(std::clamp(std::floor(value + 0.5), -scale, top));
        const auto bits = static_cast<std::uint32_t>(quantised);
        out.push_back(static_cast<std::uint8_t>(bits & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
        if (!is16)
            out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
    }
}

bool WavFileWriter::write(std::span<const float> interleaved, std::string* error)
{
    if (!open_)
        return fail(error, "The WAV file is not open");
    const auto channels = static_cast<std::size_t>(spec_.channels);
    if (interleaved.size() % channels != 0 || interleaved.size() / channels > maxFrames_ - written_) {
        abandon();
        return fail(error,
            spec_.frames ? "More audio than the WAV file was opened for"
                         : "The audio is too long for a WAV file (4 GiB)");
    }
    // Little-endian bytes in pieces, so the buffer stays small.
    std::vector<std::uint8_t> bytes;
    constexpr std::size_t kChunk = 16384;
    for (std::size_t at = 0; at < interleaved.size(); at += kChunk) {
        bytes.clear();
        encode(interleaved.subspan(at, std::min(kChunk, interleaved.size() - at)), bytes);
        stream_.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream_) {
            abandon();
            return fail(error, "Failed while writing the WAV file");
        }
    }
    written_ += interleaved.size() / channels;
    return true;
}

bool WavFileWriter::finish(std::string* error)
{
    if (!open_)
        return fail(error, "The WAV file is not open");
    if (spec_.frames && written_ != *spec_.frames) {
        abandon();
        return fail(error, "Less audio than the WAV file was opened for");
    }
    if (!spec_.frames) {
        // The header was written for 0 frames: now the length is known.
        const std::vector<std::uint8_t> header
            = encodeWavHeader(written_, spec_.channels, spec_.sampleRate, spec_.format);
        stream_.seekp(0);
        stream_.write(
            reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    }
    stream_.close();
    if (!stream_) {
        abandon();
        return fail(error, "Failed while writing the WAV file");
    }
    std::error_code ec;
    std::filesystem::rename(temporary_, target_, ec); // replaces an existing file
    if (ec) {
        abandon();
        return fail(error, "Could not put the WAV file in place: " + ec.message());
    }
    open_ = false;
    return true;
}

bool writeWavFloat32(const std::string& path, std::span<const float> interleaved, int channels,
    int sampleRate, std::string* error)
{
    if (channels < 1 || interleaved.size() % static_cast<std::size_t>(channels) != 0)
        return fail(error, "Cannot encode this audio as WAV (bad format, or larger than 4 GiB)");
    WavFileWriter writer;
    return writer.open(
               path, interleaved.size() / static_cast<std::size_t>(channels), channels, sampleRate, error)
        && writer.write(interleaved, error) && writer.finish(error);
}

} // namespace vloom
