// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "io/AudioBackend.hpp"

/// Internal: the platform audio APIs behind vloom::AudioInput, one per OS in
/// src/io/<Name>InputBackend.cpp. Kept apart from the output backends so
/// capture can never disturb playback.
namespace vloom::io {

/// Hands over `numFrames` captured interleaved float frames of `numChannels`
/// channels. Called on the backend's real-time thread: must not block or
/// allocate. `numFrames` can exceed AudioInput::kMaxBlockFrames.
using CaptureFn = void (*)(void* user, const float* interleaved, int numChannels, int numFrames) noexcept;

/// One platform capture API. Control calls come from one control thread;
/// CaptureFn is called from the backend's own real-time thread between
/// start() and stop().
class InputBackend {
public:
    InputBackend() = default;
    virtual ~InputBackend() = default;
    InputBackend(const InputBackend&) = delete;
    InputBackend& operator=(const InputBackend&) = delete;

    [[nodiscard]] virtual std::vector<BackendDeviceInfo> listInputDevices() = 0;
    /// Opens (does not start) a capture stream; `config.deviceId` empty =
    /// the system's default input. `info` says what it got. On failure
    /// fills `error` and returns false.
    virtual bool open(const BackendStreamConfig& config, CaptureFn capture, void* user,
        BackendStreamInfo& info, std::string& error) = 0;
    virtual bool start(std::string& error) = 0;
    /// Returns once CaptureFn is no longer being called.
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;
    /// True between start() and stop(), unless the device was lost (unplugged).
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
};

/// WASAPI on Windows, CoreAudio on macOS, ALSA on Linux; null elsewhere.
[[nodiscard]] std::unique_ptr<InputBackend> createPlatformInputBackend();

} // namespace vloom::io
