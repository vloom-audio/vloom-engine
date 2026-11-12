// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sample/SampleLoader.hpp"

#include "vloom/engine/sample/Resampler.hpp"

namespace vloom {

std::shared_ptr<const SampleBuffer> loadSample(
    const std::string& path, double sampleRate, std::string* error, const WavReadLimits& limits)
{
    WavReadResult read = readWavFile(path, limits);
    if (!read.buffer) {
        if (error)
            *error = read.error;
        return nullptr;
    }
    if (read.buffer->sampleRate() == sampleRate)
        return std::make_shared<const SampleBuffer>(std::move(*read.buffer));
    SampleBuffer converted = resample(*read.buffer, sampleRate);
    if (converted.sampleRate() != sampleRate) {
        if (error)
            *error = "Cannot convert '" + path + "' to the engine's sample rate";
        return nullptr;
    }
    return std::make_shared<const SampleBuffer>(std::move(converted));
}

} // namespace vloom
