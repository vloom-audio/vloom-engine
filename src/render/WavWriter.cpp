// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/WavWriter.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <exception>
#include <limits>
#include <system_error>

namespace vloom {

namespace {

constexpr std::uint16_t kFormatIeeeFloat = 3;
constexpr std::uint32_t kFmtChunkSize = 18; // WAVEFORMATEX with cbSize = 0
constexpr std::uint32_t kFactChunkSize = 4;
// "WAVE" + fmt chunk + fact chunk + data chunk header.
constexpr std::uint64_t kHeaderBytesInRiff = 4 + (8 + kFmtChunkSize) + (8 + kFactChunkSize) + 8;
constexpr std::uint64_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();

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

void appendSamples(std::vector<std::uint8_t>& out, std::span<const float> interleaved)
{
    for (const float sample : interleaved)
        putU32(out, std::bit_cast<std::uint32_t>(sample));
}

bool fail(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

std::vector<std::uint8_t> encodeWavFloat32Header(std::uint64_t frames, int channels, int sampleRate)
{
    if (channels < 1 || channels > kMaxWavChannels || sampleRate <= 0)
        return {};
    const auto blockAlign = static_cast<std::uint16_t>(channels * 4);
    const std::uint64_t byteRate = static_cast<std::uint64_t>(sampleRate) * blockAlign;
    if (byteRate > kMaxU32)
        return {};
    if (frames > kMaxU32 / blockAlign)
        return {};
    const std::uint64_t dataBytes = frames * blockAlign;
    if (kHeaderBytesInRiff + dataBytes > kMaxU32)
        return {}; // would need RF64

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(8 + kHeaderBytesInRiff));
    put(out, "RIFF");
    putU32(out, static_cast<std::uint32_t>(kHeaderBytesInRiff + dataBytes));
    put(out, "WAVE");

    put(out, "fmt ");
    putU32(out, kFmtChunkSize);
    putU16(out, kFormatIeeeFloat);
    putU16(out, static_cast<std::uint16_t>(channels));
    putU32(out, static_cast<std::uint32_t>(sampleRate));
    putU32(out, static_cast<std::uint32_t>(byteRate));
    putU16(out, blockAlign);
    putU16(out, 32); // bits per sample
    putU16(out, 0); // cbSize

    put(out, "fact");
    putU32(out, kFactChunkSize);
    putU32(out, static_cast<std::uint32_t>(frames));

    put(out, "data");
    putU32(out, static_cast<std::uint32_t>(dataBytes));
    return out;
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
    appendSamples(out, interleaved);
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
    abandon();
    const std::vector<std::uint8_t> header = encodeWavFloat32Header(frames, channels, sampleRate);
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
    framesLeft_ = frames;
    channels_ = channels;
    return true;
}

bool WavFileWriter::write(std::span<const float> interleaved, std::string* error)
{
    if (!open_)
        return fail(error, "The WAV file is not open");
    const auto channels = static_cast<std::size_t>(channels_);
    if (interleaved.size() % channels != 0 || interleaved.size() / channels > framesLeft_) {
        abandon();
        return fail(error, "More audio than the WAV file was opened for");
    }
    // Little-endian bytes in pieces, so the buffer stays small.
    std::vector<std::uint8_t> bytes;
    constexpr std::size_t kChunk = 16384;
    for (std::size_t at = 0; at < interleaved.size(); at += kChunk) {
        bytes.clear();
        appendSamples(bytes, interleaved.subspan(at, std::min(kChunk, interleaved.size() - at)));
        stream_.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream_) {
            abandon();
            return fail(error, "Failed while writing the WAV file");
        }
    }
    framesLeft_ -= interleaved.size() / channels;
    return true;
}

bool WavFileWriter::finish(std::string* error)
{
    if (!open_)
        return fail(error, "The WAV file is not open");
    if (framesLeft_ != 0) {
        abandon();
        return fail(error, "Less audio than the WAV file was opened for");
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
