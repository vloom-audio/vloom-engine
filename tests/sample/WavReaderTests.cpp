// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/render/WavWriter.hpp>
#include <vloom/engine/sample/WavReader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void putU16(Bytes& b, std::uint32_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void putU32(Bytes& b, std::uint32_t v)
{
    for (int s = 0; s < 32; s += 8)
        b.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}

void putTag(Bytes& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

void putChunk(Bytes& b, const char* tag, const Bytes& body, std::uint32_t declaredSize)
{
    putTag(b, tag);
    putU32(b, declaredSize);
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() % 2 == 1)
        b.push_back(0); // pad byte
}

void putChunk(Bytes& b, const char* tag, const Bytes& body)
{
    putChunk(b, tag, body, static_cast<std::uint32_t>(body.size()));
}

Bytes fmtBody(std::uint32_t tag, int channels, int rate, int bits)
{
    Bytes f;
    putU16(f, tag);
    putU16(f, static_cast<std::uint32_t>(channels));
    putU32(f, static_cast<std::uint32_t>(rate));
    putU32(f, static_cast<std::uint32_t>(rate * channels * bits / 8));
    putU16(f, static_cast<std::uint32_t>(channels * bits / 8));
    putU16(f, static_cast<std::uint32_t>(bits));
    return f;
}

Bytes extensibleFmtBody(std::uint32_t subTag, int channels, int rate, int bits)
{
    Bytes f = fmtBody(0xFFFE, channels, rate, bits);
    putU16(f, 22); // cbSize
    putU16(f, static_cast<std::uint32_t>(bits)); // valid bits
    putU32(f, 3); // channel mask
    putU16(f, subTag);
    const Bytes tail{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    f.insert(f.end(), tail.begin(), tail.end());
    return f;
}

/// RIFF file with the given chunks (already encoded).
Bytes riff(const Bytes& chunks)
{
    Bytes b;
    putTag(b, "RIFF");
    putU32(b, static_cast<std::uint32_t>(4 + chunks.size()));
    putTag(b, "WAVE");
    b.insert(b.end(), chunks.begin(), chunks.end());
    return b;
}

Bytes wav(const Bytes& fmt, const Bytes& data)
{
    Bytes chunks;
    putChunk(chunks, "fmt ", fmt);
    putChunk(chunks, "data", data);
    return riff(chunks);
}

} // namespace

TEST_CASE("16-bit PCM is decoded to planar floats", "[sample]")
{
    Bytes data;
    for (const int v : {0, 16384, -32768, 32767}) // L R L R
        putU16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    const auto result = vloom::decodeWav(wav(fmtBody(1, 2, 44100, 16), data));
    INFO(result.error);
    REQUIRE(result.buffer);
    const auto& b = *result.buffer;
    CHECK(b.channels() == 2);
    CHECK(b.frames() == 2);
    CHECK(b.sampleRate() == 44100.0);
    CHECK(b.channel(0)[0] == 0.0f);
    CHECK(b.channel(1)[0] == 0.5f);
    CHECK(b.channel(0)[1] == -1.0f);
    CHECK(b.channel(1)[1] == 32767.0f / 32768.0f);
}

TEST_CASE("8, 24 and 32-bit PCM are scaled to -1..1", "[sample]")
{
    {
        const Bytes data{0, 128, 255};
        const auto r = vloom::decodeWav(wav(fmtBody(1, 1, 8000, 8), data));
        REQUIRE(r.buffer);
        CHECK(r.buffer->channel(0)[0] == -1.0f);
        CHECK(r.buffer->channel(0)[1] == 0.0f);
        CHECK(r.buffer->channel(0)[2] == 127.0f / 128.0f);
    }
    {
        const Bytes data{0x00, 0x00, 0x80, 0x00, 0x00, 0x40}; // -8388608, +4194304
        const auto r = vloom::decodeWav(wav(fmtBody(1, 1, 48000, 24), data));
        REQUIRE(r.buffer);
        CHECK(r.buffer->channel(0)[0] == -1.0f);
        CHECK(r.buffer->channel(0)[1] == 0.5f);
    }
    {
        Bytes data;
        putU32(data, 0x80000000U);
        putU32(data, 0x40000000U);
        const auto r = vloom::decodeWav(wav(fmtBody(1, 1, 48000, 32), data));
        REQUIRE(r.buffer);
        CHECK(r.buffer->channel(0)[0] == -1.0f);
        CHECK(r.buffer->channel(0)[1] == 0.5f);
    }
}

TEST_CASE("Float WAV is read exactly; NaN and infinity become silence", "[sample]")
{
    Bytes data;
    for (const float v :
        {0.25f, -2.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
        putU32(data, std::bit_cast<std::uint32_t>(v));
    const auto r = vloom::decodeWav(wav(fmtBody(3, 1, 48000, 32), data));
    REQUIRE(r.buffer);
    CHECK(r.buffer->channel(0)[0] == 0.25f);
    CHECK(r.buffer->channel(0)[1] == -2.0f); // over-range floats are kept
    CHECK(r.buffer->channel(0)[2] == 0.0f);
    CHECK(r.buffer->channel(0)[3] == 0.0f);

    Bytes data64;
    const auto bits = std::bit_cast<std::uint64_t>(0.125);
    putU32(data64, static_cast<std::uint32_t>(bits));
    putU32(data64, static_cast<std::uint32_t>(bits >> 32));
    const auto r64 = vloom::decodeWav(wav(fmtBody(3, 1, 48000, 64), data64));
    REQUIRE(r64.buffer);
    CHECK(r64.buffer->channel(0)[0] == 0.125f);
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE files are read", "[sample]")
{
    Bytes data{0x00, 0x00, 0x40, 0x00, 0x00, 0xC0}; // stereo 24-bit: +0.5, -0.5
    const auto r = vloom::decodeWav(wav(extensibleFmtBody(1, 2, 96000, 24), data));
    INFO(r.error);
    REQUIRE(r.buffer);
    CHECK(r.buffer->sampleRate() == 96000.0);
    CHECK(r.buffer->channel(0)[0] == 0.5f);
    CHECK(r.buffer->channel(1)[0] == -0.5f);
}

TEST_CASE("Metadata chunks are skipped, odd sizes padded", "[sample]")
{
    Bytes chunks;
    putChunk(chunks, "bext", Bytes(7, 0xEE)); // odd size: one pad byte follows
    putChunk(chunks, "LIST", Bytes(12, 0x41));
    putChunk(chunks, "fmt ", fmtBody(1, 1, 22050, 16));
    putChunk(chunks, "cue ", Bytes(3, 0x01));
    Bytes data;
    putU16(data, 16384);
    putChunk(chunks, "data", data);
    const auto r = vloom::decodeWav(riff(chunks));
    INFO(r.error);
    REQUIRE(r.buffer);
    CHECK(r.buffer->frames() == 1);
    CHECK(r.buffer->channel(0)[0] == 0.5f);
}

TEST_CASE("A data chunk longer than the file is read to the last whole frame", "[sample]")
{
    Bytes data;
    for (int i = 0; i < 5; ++i)
        putU16(data, 1000); // 5 samples = 2.5 stereo frames
    for (const std::uint32_t declared : {1000U, 0xFFFFFFFFU}) {
        Bytes chunks;
        putChunk(chunks, "fmt ", fmtBody(1, 2, 48000, 16));
        putChunk(chunks, "data", data, declared);
        const auto r = vloom::decodeWav(riff(chunks)); // the file ends inside the data
        INFO(r.error);
        REQUIRE(r.buffer);
        CHECK(r.buffer->frames() == 2);
    }
}

TEST_CASE("Broken or unsupported WAV files are rejected with a message", "[sample]")
{
    const Bytes pcm = fmtBody(1, 2, 48000, 16);
    const Bytes frame{0, 0, 0, 0};
    const auto rejects = [](const Bytes& file) {
        const auto r = vloom::decodeWav(file);
        CHECK_FALSE(r.buffer);
        CHECK_FALSE(r.error.empty());
    };

    rejects({});
    rejects(Bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' '});
    {
        Bytes chunks; // no fmt
        putChunk(chunks, "data", frame);
        rejects(riff(chunks));
    }
    {
        Bytes chunks; // no data
        putChunk(chunks, "fmt ", pcm);
        rejects(riff(chunks));
    }
    {
        Bytes chunks; // two fmt chunks
        putChunk(chunks, "fmt ", pcm);
        putChunk(chunks, "fmt ", pcm);
        putChunk(chunks, "data", frame);
        rejects(riff(chunks));
    }
    rejects(wav(fmtBody(2, 2, 48000, 16), frame)); // ADPCM
    rejects(wav(fmtBody(1, 0, 48000, 16), frame)); // no channels
    rejects(wav(fmtBody(1, 9, 48000, 16), frame)); // too many channels
    rejects(wav(fmtBody(1, 2, 0, 16), frame)); // no rate
    rejects(wav(fmtBody(1, 2, 48000, 12), frame)); // odd bit depth
    rejects(wav(fmtBody(3, 2, 48000, 16), frame)); // 16-bit float
    {
        Bytes bad = pcm;
        bad[12] = 3; // block align 3 instead of 4
        rejects(wav(bad, frame));
    }
    {
        Bytes ext = extensibleFmtBody(1, 2, 48000, 16);
        ext.back() ^= 0xFF; // not a KSDATAFORMAT GUID
        rejects(wav(ext, frame));
    }
    {
        Bytes chunks; // fmt chunk declares more than the file has
        putChunk(chunks, "fmt ", pcm, 40);
        rejects(riff(chunks));
    }
    {
        Bytes shortFmt(pcm.begin(), pcm.begin() + 14);
        rejects(wav(shortFmt, frame));
    }
}

TEST_CASE("Size limits are checked before decoding", "[sample]")
{
    Bytes data(4 * 100, 0); // 100 stereo 16-bit frames
    const Bytes file = wav(fmtBody(1, 2, 48000, 16), data);

    vloom::WavReadLimits limits;
    CHECK(vloom::decodeWav(file, limits).buffer);
    limits.maxFileBytes = file.size() - 1;
    CHECK_FALSE(vloom::decodeWav(file, limits).buffer);

    limits = {};
    limits.maxFrames = 99;
    CHECK_FALSE(vloom::decodeWav(file, limits).buffer);

    limits = {};
    limits.maxDecodedBytes = 100 * 2 * sizeof(float) - 1;
    CHECK_FALSE(vloom::decodeWav(file, limits).buffer);

    limits = {};
    limits.maxChannels = 1;
    CHECK_FALSE(vloom::decodeWav(file, limits).buffer);
}

TEST_CASE("Files from WavWriter read back bit for bit", "[sample]")
{
    std::vector<float> samples;
    for (int i = 0; i < 300; ++i)
        samples.push_back(static_cast<float>(i) / 300.0f - 0.5f);
    const auto r = vloom::decodeWav(vloom::encodeWavFloat32(samples, 3, 32000));
    REQUIRE(r.buffer);
    REQUIRE(r.buffer->channels() == 3);
    REQUIRE(r.buffer->frames() == 100);
    for (int i = 0; i < 100; ++i)
        for (int ch = 0; ch < 3; ++ch)
            CHECK(r.buffer->channel(ch)[i] == samples[static_cast<std::size_t>(i * 3 + ch)]);
}

TEST_CASE("Corrupted and truncated files never read out of bounds", "[sample]")
{
    // Under ASan (CI) any out-of-bounds read fails this test. Deterministic seed.
    Bytes chunks;
    putChunk(chunks, "LIST", Bytes(9, 0x20));
    putChunk(chunks, "fmt ", extensibleFmtBody(1, 2, 48000, 24));
    putChunk(chunks, "data", Bytes(6 * 40, 0x11));
    const Bytes valid = riff(chunks);

    std::mt19937 random(12345);
    for (int round = 0; round < 3000; ++round) {
        Bytes file = valid;
        const int flips = 1 + static_cast<int>(random() % 8);
        for (int f = 0; f < flips; ++f)
            file[random() % file.size()] = static_cast<std::uint8_t>(random());
        if (random() % 3 == 0)
            file.resize(random() % file.size());
        const auto r = vloom::decodeWav(file);
        if (r.buffer)
            CHECK(r.buffer->frames() * r.buffer->channels() <= static_cast<std::int64_t>(file.size()));
        else
            CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("WAV files are read from UTF-8 paths", "[sample]")
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path folder
        = std::filesystem::temp_directory_path() / ("vloom_wavread_" + std::to_string(stamp));
    std::filesystem::create_directories(folder);
    const std::string path = folder.string() + "/kick-\xE0\xB8\x81.wav"; // Thai "ก"
    const std::vector<float> samples{0.5f, -0.5f};
    REQUIRE(vloom::writeWavFloat32(path, samples, 1, 48000));

    const auto r = vloom::readWavFile(path);
    INFO(r.error);
    REQUIRE(r.buffer);
    CHECK(r.buffer->frames() == 2);
    CHECK(r.buffer->channel(0)[1] == -0.5f);

    CHECK_FALSE(vloom::readWavFile(folder.string() + "/missing.wav").buffer);
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}
