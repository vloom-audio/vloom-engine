// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/OfflineRenderer.hpp"

#include "vloom/engine/Engine.hpp"
#include "vloom/engine/render/WavWriter.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace vloom {

namespace {

/// More channels than any device has: the engine fills those past 8 with
/// silence anyway.
constexpr int kMaxRenderChannels = 64;
/// renderInterleaved() holds the whole render: past this it returns nothing.
constexpr std::int64_t kMaxInterleavedSamples = std::int64_t{1} << 30; // 4 GiB of float

int channelsOf(const OfflineRenderOptions& options)
{
    return std::clamp(options.channels, 1, kMaxRenderChannels);
}

/// Renders `totalFrames` in blocks, handing each interleaved block to `sink`
/// (which returns false to stop). Returns false if the sink stopped it.
template <typename Sink>
bool renderBlocks(Engine& engine, const OfflineRenderOptions& options, std::int64_t totalFrames, Sink&& sink)
{
    const int channels = channelsOf(options);
    const int blockSize = std::clamp(options.blockSize, 1, kMaxBlockFrames);
    const auto width = static_cast<std::size_t>(channels);
    const auto length = static_cast<std::size_t>(blockSize);
    std::vector<float> planar(width * length);
    std::vector<float*> ptrs(width);
    for (std::size_t ch = 0; ch < width; ++ch)
        ptrs[ch] = planar.data() + ch * length;
    std::vector<float> interleaved(width * length);

    for (std::int64_t done = 0; done < totalFrames;) {
        const auto block = static_cast<int>(std::min<std::int64_t>(blockSize, totalFrames - done));
        engine.process(ptrs.data(), channels, block);
        for (std::size_t i = 0; i < static_cast<std::size_t>(block); ++i)
            for (std::size_t ch = 0; ch < width; ++ch)
                interleaved[i * width + ch] = ptrs[ch][i];
        if (!sink(std::span<const float>(interleaved.data(), static_cast<std::size_t>(block) * width)))
            return false;
        done += block;
    }
    return true;
}

} // namespace

std::int64_t offlineRenderFrames(const Engine& engine, const OfflineRenderOptions& options)
{
    const double frames = options.seconds * engine.sampleRate();
    // Past this no buffer or WAV file could hold it anyway; it keeps the
    // conversion below defined.
    constexpr double kMaxFrames = 1e15;
    if (!std::isfinite(frames) || frames <= 0.0)
        return 0;
    return static_cast<std::int64_t>(std::llround(std::min(frames, kMaxFrames)));
}

std::vector<float> renderInterleaved(Engine& engine, const OfflineRenderOptions& options)
{
    const std::int64_t totalFrames = offlineRenderFrames(engine, options);
    if (totalFrames > kMaxInterleavedSamples / channelsOf(options))
        return {}; // too long to hold: renderToWav() streams it
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>(totalFrames) * static_cast<std::size_t>(channelsOf(options)));
    renderBlocks(engine, options, totalFrames, [&result](std::span<const float> block) {
        result.insert(result.end(), block.begin(), block.end());
        return true;
    });
    return result;
}

bool renderToWav(
    Engine& engine, const std::string& path, const OfflineRenderOptions& options, std::string* error)
{
    const std::int64_t totalFrames = offlineRenderFrames(engine, options);
    WavFileWriter writer;
    // open() refuses a render too long for WAV before anything is rendered.
    if (!writer.open(path, static_cast<std::uint64_t>(totalFrames), channelsOf(options),
            static_cast<int>(std::lround(engine.sampleRate())), error))
        return false;
    return renderBlocks(engine, options, totalFrames, [&](std::span<const float> block) {
        return writer.write(block, error);
    }) && writer.finish(error);
}

} // namespace vloom
