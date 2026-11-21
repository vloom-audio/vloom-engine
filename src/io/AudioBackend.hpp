// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <functional>
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
    bool exclusive = false; ///< WASAPI exclusive mode; other backends ignore it
    int inputChannels = 0; ///< ASIO: driver inputs opened with the output; others ignore it
};

/// What open() actually got.
struct BackendStreamInfo {
    int sampleRate = 0;
    int channels = 0;
    std::string deviceName;
    bool exclusive = false; ///< the stream really runs exclusive
    int bufferFrames = 0; ///< the period the stream got; 0 = the backend does not say
    int inputLatency = 0; ///< frames the driver reports for its input; 0 = unknown
    int outputLatency = 0; ///< frames the driver reports for its output; 0 = unknown
};

/// Fills `numFrames` interleaved float frames of `numChannels` channels.
/// Called on the backend's real-time thread: must not block or allocate.
/// `numFrames` can exceed AudioDevice::kMaxBlockFrames.
using RenderFn = void (*)(void* user, float* interleaved, int numChannels, int numFrames) noexcept;

/// Keeps device-list notifications coming while it lives (see
/// AudioBackend::watchDeviceList()).
class DeviceListSubscription {
public:
    DeviceListSubscription() = default;
    virtual ~DeviceListSubscription() = default;
    DeviceListSubscription(const DeviceListSubscription&) = delete;
    DeviceListSubscription& operator=(const DeviceListSubscription&) = delete;
};

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

    /// Opens the driver's own settings dialog, where the API has one (ASIO).
    virtual bool showControlPanel(std::string& error)
    {
        error = "This audio API has no control panel";
        return false;
    }

    /// True once the driver asked for the open stream to be reopened (ASIO
    /// reset, buffer-size, resync or latency messages).
    [[nodiscard]] virtual bool resetRequested() const noexcept { return false; }

    /// The open stream's current latencies, asking the driver again if it
    /// reported a change (ASIO). False when the backend does not report
    /// them: BackendStreamInfo's values stand.
    virtual bool refreshLatency(int& input, int& output) noexcept
    {
        (void)input;
        (void)output;
        return false;
    }

    /// The period to reopen with after resetRequested(): the driver's new
    /// size after a buffer-size change (ASIO). 0 = the backend does not say.
    [[nodiscard]] virtual int reopenBufferFrames() noexcept { return 0; }

    /// Calls `onChange` when outputs are added or removed or the default
    /// output changes. It runs on an OS thread, so it must be quick and
    /// thread-safe. Notifications stop when the subscription is destroyed;
    /// create and destroy it on the same thread. Null when the platform cannot
    /// notify (callers then list devices again from time to time).
    [[nodiscard]] virtual std::unique_ptr<DeviceListSubscription> watchDeviceList(
        std::function<void()> onChange)
    {
        (void)onChange;
        return nullptr;
    }
};

/// WASAPI on Windows, CoreAudio on macOS, ALSA on Linux.
[[nodiscard]] std::unique_ptr<AudioBackend> createPlatformBackend();

} // namespace vloom::io
