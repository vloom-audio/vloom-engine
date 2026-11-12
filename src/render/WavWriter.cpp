// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/WavWriter.hpp"

#include <bit>
#include <filesystem>
#include <fstream>
#include <limits>

namespace vloom {

namespace {

constexpr std::uint16_t kFormatIeeeFloat = 3;
constexpr std::uint32_t kFmtChunkSize = 18; // WAVEFORMATEX with cbSize = 0
constexpr std::uint32_t kFactChunkSize = 4;
// "WAVE" + fmt chunk + fact chunk + data chunk header.
constexpr std::uint64_t kHeaderBytesInRiff = 4 + (8 + kFmtChunkSize) + (8 + kFactChunkSize) + 8;

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

} // namespace

std::vector<std::uint8_t> encodeWavFloat32(std::span<const float> interleaved, int channels, int sampleRate)
{
    if (channels < 1 || channels > std::numeric_limits<std::uint16_t>::max() / 4 || sampleRate <= 0
        || interleaved.size() % static_cast<std::size_t>(channels) != 0)
        return {};
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(interleaved.size()) * 4;
    if (kHeaderBytesInRiff + dataBytes > std::numeric_limits<std::uint32_t>::max())
        return {}; // would need RF64

    const auto frames = static_cast<std::uint32_t>(interleaved.size() / static_cast<std::size_t>(channels));
    const auto blockAlign = static_cast<std::uint16_t>(channels * 4);

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(8 + kHeaderBytesInRiff + dataBytes));
    put(out, "RIFF");
    putU32(out, static_cast<std::uint32_t>(kHeaderBytesInRiff + dataBytes));
    put(out, "WAVE");

    put(out, "fmt ");
    putU32(out, kFmtChunkSize);
    putU16(out, kFormatIeeeFloat);
    putU16(out, static_cast<std::uint16_t>(channels));
    putU32(out, static_cast<std::uint32_t>(sampleRate));
    putU32(out, static_cast<std::uint32_t>(sampleRate) * blockAlign);
    putU16(out, blockAlign);
    putU16(out, 32); // bits per sample
    putU16(out, 0); // cbSize

    put(out, "fact");
    putU32(out, kFactChunkSize);
    putU32(out, frames);

    put(out, "data");
    putU32(out, static_cast<std::uint32_t>(dataBytes));
    for (const float sample : interleaved)
        putU32(out, std::bit_cast<std::uint32_t>(sample));
    return out;
}

bool writeWavFloat32(const std::string& path, std::span<const float> interleaved, int channels,
    int sampleRate, std::string* error)
{
    const auto fail = [&](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };
    const std::vector<std::uint8_t> bytes = encodeWavFloat32(interleaved, channels, sampleRate);
    if (bytes.empty())
        return fail("Cannot encode this audio as WAV (bad format, or larger than 4 GiB)");

    // UTF-8 path on every platform (Windows paths are wide).
    const std::filesystem::path file(std::u8string(path.begin(), path.end()));
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream)
        return fail("Could not open '" + path + "' for writing");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        return fail("Failed while writing '" + path + "'");
    return true;
}

} // namespace vloom
