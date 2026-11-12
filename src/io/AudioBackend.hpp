// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <memory>
#include <string>
#include <vector>

/// Internal: the platform audio APIs behind vloom::AudioDevice. Not part of
/// the public API; each backend lives in its own src/io/<Name>Backend.cpp
/// and only that file includes the platform's headers.
namespace vloom::io {

struct BackendDeviceInfo {
    std::string id; ///< opaque, passed back to open(); stable while the device is plugged in
    std::string name; ///< UTF-8, for display
    bool isDefault = false;
};

struct BackendStreamConfig {
    std::string deviceId; ///< empty = the system default output
    int sampleRate = 48000; ///< 0 = the device's own rate
    int bufferFrames = 256; ///< requested period; the backend may round it
    int channels = 2;
};

/// What open() actually got.
struct BackendStreamInfo {
    int sampleRate = 0;
    int channels = 0;
    std::string deviceName;
};

/// Fills `numFrames` interleaved float frames of `numChannels` channels.
/// Called on the backend's real-time thread: must not block or allocate.
/// `numFrames` can exceed AudioDevice::kMaxBlockFrames.
using RenderFn = void (*)(void* user, float* interleaved, int numChannels, int numFrames) noexcept;

/// One platform audio API. Control calls come from one control thread;
/// RenderFn is called from the backend's own real-time thread between
/// start() and stop().
class AudioBackend {
public:
    AudioBackend() = default;
    virtual ~AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    [[nodiscard]] virtual std::vector<BackendDeviceInfo> listOutputDevices() = 0;

    /// Opens (does not start) an output stream. The backend converts to the
    /// requested rate and channel count where the OS allows; `info` says
    /// what it got. On failure fills `error` and returns false.
    virtual bool open(const BackendStreamConfig& config, RenderFn render, void* user, BackendStreamInfo& info,
        std::string& error) = 0;
    virtual bool start(std::string& error) = 0;
    /// Returns once RenderFn is no longer being called.
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;
    /// True between start() and stop(), unless the device was lost (unplugged).
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
};

/// WASAPI on Windows, CoreAudio on macOS, ALSA on Linux.
[[nodiscard]] std::unique_ptr<AudioBackend> createPlatformBackend();

} // namespace vloom::io
