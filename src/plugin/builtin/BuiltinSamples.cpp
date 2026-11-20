// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/builtin/BuiltinSamples.hpp"

#include "vloom/engine/sample/WavReader.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace vloom::builtin {

namespace {

constexpr std::array<std::uint8_t, 4> kPathsMagic{'S', 'M', 'P', '1'};

void put32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t get32(std::span<const std::uint8_t> in) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
    return value;
}

void report(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}

std::shared_ptr<const dsp::SampleBuffer> read(
    const std::string& path, const SampleFileResolver& resolver, std::string* error)
{
    if (path.empty()) {
        report(error, "No file given");
        return nullptr;
    }
    std::string file = path;
    if (resolver) {
        std::string why;
        file = resolver(path, &why);
        if (file.empty()) {
            report(error, why.empty() ? "Cannot read '" + path + "'" : why);
            return nullptr;
        }
    }
    WavReadResult wav = readWavFile(file);
    if (!wav.buffer) {
        report(error, wav.error.empty() ? "Cannot read '" + path + "'" : wav.error);
        return nullptr;
    }
    const SampleBuffer& decoded = *wav.buffer;
    if (decoded.empty() || decoded.frames() > std::numeric_limits<int>::max()) {
        report(error, "'" + path + "' holds no audio the sampler can play");
        return nullptr;
    }
    const int frames = static_cast<int>(decoded.frames());
    std::shared_ptr<const dsp::SampleBuffer> buffer;
    if (decoded.channels() <= dsp::SampleBuffer::kMaxChannels) {
        std::array<const float*, dsp::SampleBuffer::kMaxChannels> channels{};
        for (int ch = 0; ch < decoded.channels(); ++ch)
            channels[static_cast<std::size_t>(ch)] = decoded.channel(ch);
        buffer = dsp::SampleBuffer::copy(channels.data(), decoded.channels(), frames, decoded.sampleRate());
    } else {
        // More than stereo: even channels to the left, odd to the right.
        std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
        std::vector<float> right(static_cast<std::size_t>(frames), 0.0f);
        const int count = decoded.channels();
        const float leftScale = 1.0f / static_cast<float>((count + 1) / 2);
        const float rightScale = 1.0f / static_cast<float>(count / 2);
        for (int ch = 0; ch < count; ++ch) {
            const float* in = decoded.channel(ch);
            std::vector<float>& out = ch % 2 == 0 ? left : right;
            const float scale = ch % 2 == 0 ? leftScale : rightScale;
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] += in[i] * scale;
        }
        const std::array<const float*, 2> channels{left.data(), right.data()};
        buffer = dsp::SampleBuffer::copy(channels.data(), 2, frames, decoded.sampleRate());
    }
    if (buffer == nullptr)
        report(error, "'" + path + "' holds no audio the sampler can play");
    return buffer;
}

} // namespace

std::shared_ptr<const SampleAudio> sampleAudioOf(std::shared_ptr<const dsp::SampleBuffer> buffer)
{
    if (buffer == nullptr)
        return nullptr;
    // The view and the buffer it reads in one allocation; the aliasing
    // pointer hands out the view and keeps both alive.
    struct Held {
        SampleAudio audio;
        std::shared_ptr<const dsp::SampleBuffer> buffer;
    };
    try {
        auto held = std::make_shared<Held>();
        SampleAudio& audio = held->audio;
        audio.numChannels = std::clamp(buffer->numChannels(), 0, SampleAudio::kMaxChannels);
        audio.frames = buffer->frames();
        audio.sampleRate = buffer->sampleRate();
        for (int ch = 0; ch < audio.numChannels; ++ch)
            audio.channels[static_cast<std::size_t>(ch)] = buffer->channel(ch);
        held->buffer = std::move(buffer);
        return std::shared_ptr<const SampleAudio>(held, &held->audio);
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::shared_ptr<const dsp::SampleBuffer> readSampleFile(
    const std::string& path, const SampleFileResolver& resolver, std::string* error)
{
    try {
        return read(path, resolver, error);
    } catch (const std::bad_alloc&) {
        try {
            report(error, "Not enough memory to load the sample");
        } catch (...) {
        }
    } catch (const std::exception&) {
        try {
            report(error, "Could not load the sample");
        } catch (...) {
        }
    }
    return nullptr;
}

void appendSamplePaths(std::vector<std::uint8_t>& state, std::span<const std::string> paths)
{
    std::uint32_t count = 0;
    for (const std::string& path : paths)
        count += path.empty() ? 0U : 1U;
    state.insert(state.end(), kPathsMagic.begin(), kPathsMagic.end());
    put32(state, count);
    for (std::size_t slot = 0; slot < paths.size(); ++slot) {
        const std::string& path = paths[slot];
        if (path.empty())
            continue;
        const auto length
            = static_cast<std::uint32_t>(std::min<std::size_t>(path.size(), kMaxSamplePathBytes));
        put32(state, static_cast<std::uint32_t>(slot));
        put32(state, length);
        state.insert(state.end(), path.begin(), path.begin() + length);
    }
}

bool readSamplePaths(std::span<const std::uint8_t> part, int slots, std::vector<std::string>& paths)
{
    paths.assign(static_cast<std::size_t>(std::max(slots, 0)), std::string());
    if (part.size() < kPathsMagic.size() + 4
        || !std::equal(kPathsMagic.begin(), kPathsMagic.end(), part.begin()))
        return false;
    const std::uint32_t count = get32(part.subspan(kPathsMagic.size()));
    std::span<const std::uint8_t> rest = part.subspan(kPathsMagic.size() + 4);
    for (std::uint32_t entry = 0; entry < count; ++entry) {
        if (rest.size() < 8)
            return false;
        const std::uint32_t slot = get32(rest);
        const std::uint32_t length = get32(rest.subspan(4));
        rest = rest.subspan(8);
        if (length > kMaxSamplePathBytes || rest.size() < length)
            return false;
        if (slot < paths.size()) // unknown: from a version with more slots
            paths[slot].assign(reinterpret_cast<const char*>(rest.data()), length);
        rest = rest.subspan(length);
    }
    return rest.empty();
}

} // namespace vloom::builtin
