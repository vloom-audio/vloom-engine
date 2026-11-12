// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/sample/SampleBuffer.hpp"
#include "vloom/engine/sample/WavReader.hpp"

#include <memory>
#include <string>

namespace vloom {

/// Reads a sample file (WAV) and resamples it to `sampleRate`, ready for a
/// ChannelState. Slow (disk, resampling): call it on a control or background
/// thread, never the audio thread. Returns null and fills `error` on failure.
[[nodiscard]] std::shared_ptr<const SampleBuffer> loadSample(const std::string& path, double sampleRate,
    std::string* error = nullptr, const WavReadLimits& limits = {});

} // namespace vloom
