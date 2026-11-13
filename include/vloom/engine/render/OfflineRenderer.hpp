// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/render/WavWriter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vloom {

class Engine;

struct OfflineRenderOptions {
    double seconds = 4.0; ///< negative or non-finite renders nothing
    int blockSize = 512; ///< clamped to 1 .. kMaxBlockFrames
    int channels = 2; ///< clamped to 1 .. 64 (the engine renders 8, the rest silent)
    WavFormat format = WavFormat::Float32; ///< renderToWav() only
    bool dither = true; ///< renderToWav() with an integer format
};

/// Frames `options` asks for at the engine's rate: 0 for a negative or
/// non-finite length.
[[nodiscard]] std::int64_t offlineRenderFrames(const Engine& engine, const OfflineRenderOptions& options);

/// Renders the engine faster than real time into an interleaved buffer.
/// The engine must already be prepared at the desired sample rate. The
/// buffer holds the whole render: longer than 2^30 samples (4 GiB) it is
/// empty; use renderToWav() for long renders.
std::vector<float> renderInterleaved(Engine& engine, const OfflineRenderOptions& options);

/// Renders the engine into a WAV file (options.format), a block at a time (the
/// whole render is never in memory). A render that would not fit in a WAV
/// file (4 GiB) is refused before anything is rendered; a failed one leaves
/// no file behind.
bool renderToWav(Engine& engine, const std::string& path, const OfflineRenderOptions& options,
    std::string* error = nullptr);

} // namespace vloom
