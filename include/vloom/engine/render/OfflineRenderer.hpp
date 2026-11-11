// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <string>
#include <vector>

namespace vloom {

class Engine;

struct OfflineRenderOptions {
    double seconds = 4.0;
    int blockSize = 512;
    int channels = 2;
};

/// Renders the engine faster than real time into an interleaved buffer.
/// The engine must already be prepared at the desired sample rate.
std::vector<float> renderInterleaved(Engine& engine, const OfflineRenderOptions& options);

/// Renders the engine into a 32-bit float WAV file.
bool renderToWav(Engine& engine, const std::string& path, const OfflineRenderOptions& options,
    std::string* error = nullptr);

} // namespace vloom
