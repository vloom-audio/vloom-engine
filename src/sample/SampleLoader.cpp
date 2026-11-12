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
    // The limits applied to the file must hold for what stays in memory too.
    const double frames = static_cast<double>(read.buffer->frames()) * sampleRate / read.buffer->sampleRate();
    const double bytes = frames * read.buffer->channels() * static_cast<double>(sizeof(float));
    if (!(frames <= static_cast<double>(limits.maxFrames))
        || bytes > static_cast<double>(limits.maxDecodedBytes)) {
        if (error)
            *error = "'" + path + "' is too long at the engine's sample rate";
        return nullptr;
    }
    SampleBuffer converted = resample(*read.buffer, sampleRate);
    if (converted.sampleRate() != sampleRate) {
        if (error)
            *error = "Cannot convert '" + path + "' to the engine's sample rate";
        return nullptr;
    }
    return std::make_shared<const SampleBuffer>(std::move(converted));
}

} // namespace vloom
