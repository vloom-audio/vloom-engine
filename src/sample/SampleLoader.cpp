// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sample/SampleLoader.hpp"

#include "vloom/engine/sample/Resampler.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <string>

namespace vloom {

namespace {

/// What loading may grow a file to, at most: resampling a file recorded at a
/// very low rate up to the engine's would otherwise turn a few megabytes
/// into a gigabyte (and tens of seconds of work).
constexpr double kMaxGrowth = 16.0;
constexpr double kAlwaysAllowedBytes = 64.0 * 1024 * 1024;

std::shared_ptr<const SampleBuffer> load(
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
    const double decodedBytes = static_cast<double>(read.buffer->frames()) * read.buffer->channels()
        * static_cast<double>(sizeof(float));
    if (!(frames <= static_cast<double>(limits.maxFrames))
        || bytes > static_cast<double>(limits.maxDecodedBytes)
        || bytes > std::max(kAlwaysAllowedBytes, kMaxGrowth * decodedBytes)) {
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

/// Sets `error` without throwing (it is used while handling bad_alloc).
void report(std::string* error, const char* message) noexcept
{
    if (error == nullptr)
        return;
    try {
        *error = message;
    } catch (...) {
        error->clear();
    }
}

} // namespace

std::shared_ptr<const SampleBuffer> loadSample(
    const std::string& path, double sampleRate, std::string* error, const WavReadLimits& limits)
{
    // Run off the audio thread: a failure is reported, never thrown.
    try {
        return load(path, sampleRate, error, limits);
    } catch (const std::bad_alloc&) {
        report(error, "Not enough memory to load the sample");
    } catch (const std::exception&) {
        report(error, "Could not load the sample");
    }
    return nullptr;
}

} // namespace vloom
