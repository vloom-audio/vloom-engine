// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/OfflineRenderer.hpp"

#include "vloom/engine/Engine.hpp"
#include "vloom/engine/render/WavWriter.hpp"

#include <algorithm>
#include <cmath>

namespace vloom {

std::vector<float> renderInterleaved(Engine& engine, const OfflineRenderOptions& options)
{
    const int channels = std::max(options.channels, 1);
    const int blockSize = std::max(options.blockSize, 1);
    const auto totalFrames = static_cast<std::int64_t>(std::llround(options.seconds * engine.sampleRate()));

    std::vector<float> planar(static_cast<std::size_t>(channels) * blockSize);
    std::vector<float*> ptrs(static_cast<std::size_t>(channels));
    for (int ch = 0; ch < channels; ++ch)
        ptrs[static_cast<std::size_t>(ch)] = planar.data() + static_cast<std::size_t>(ch) * blockSize;

    std::vector<float> result(static_cast<std::size_t>(totalFrames) * channels);

    for (std::int64_t done = 0; done < totalFrames;) {
        const auto block = static_cast<int>(std::min<std::int64_t>(blockSize, totalFrames - done));
        engine.process(ptrs.data(), channels, block);
        for (int i = 0; i < block; ++i)
            for (int ch = 0; ch < channels; ++ch)
                result[static_cast<std::size_t>((done + i) * channels + ch)]
                    = ptrs[static_cast<std::size_t>(ch)][i];
        done += block;
    }
    return result;
}

bool renderToWav(
    Engine& engine, const std::string& path, const OfflineRenderOptions& options, std::string* error)
{
    const auto samples = renderInterleaved(engine, options);
    return writeWavFloat32(path, samples, std::max(options.channels, 1),
        static_cast<int>(std::lround(engine.sampleRate())), error);
}

} // namespace vloom
