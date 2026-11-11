// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/render/OfflineRenderer.hpp"

#include "vloom/engine/Engine.hpp"

#include <miniaudio.h>

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
    const int channels = std::max(options.channels, 1);

    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32,
        static_cast<ma_uint32>(channels), static_cast<ma_uint32>(engine.sampleRate()));

    ma_encoder encoder;
    if (ma_encoder_init_file(path.c_str(), &cfg, &encoder) != MA_SUCCESS) {
        if (error)
            *error = "Could not open '" + path + "' for writing";
        return false;
    }

    const auto frames = static_cast<ma_uint64>(samples.size() / static_cast<std::size_t>(channels));
    ma_uint64 written = 0;
    const ma_result res = ma_encoder_write_pcm_frames(&encoder, samples.data(), frames, &written);
    ma_encoder_uninit(&encoder);

    if (res != MA_SUCCESS || written != frames) {
        if (error)
            *error = "Failed while writing '" + path + "'";
        return false;
    }
    return true;
}

} // namespace vloom
