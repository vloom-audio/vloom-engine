// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vloom {

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
};

struct AudioDeviceConfig {
    int deviceIndex = -1; ///< index into listOutputDevices(), -1 = system default
    int sampleRate = 48000; ///< 0 = device native rate
    int bufferFrames = 256; ///< requested period size
    int channels = 2;
};

/// A hardware output device, through each OS's own audio API: WASAPI
/// (shared, event driven) on Windows, CoreAudio (HAL output unit) on macOS,
/// ALSA on Linux (which also reaches PulseAudio and PipeWire).
///
/// The callback runs on the real-time audio thread and receives
/// non-interleaved float buffers of at most kMaxBlockFrames frames. When the
/// device runs at another rate or channel count the OS converts, so
/// sampleRate() reports the rate the callback is driven at.
///
/// If the device is lost while running (unplugged), isRunning() turns false.
class AudioDevice {
public:
    using Callback = std::function<void(float* const* out, int numChannels, int numFrames)>;

    /// Largest block the callback will ever receive.
    static constexpr int kMaxBlockFrames = 4096;

    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    [[nodiscard]] std::vector<AudioDeviceInfo> listOutputDevices();

    /// Opens (but does not start) the device. On failure returns false and fills `error`.
    bool open(const AudioDeviceConfig& config, Callback callback, std::string* error = nullptr);
    bool start(std::string* error = nullptr);
    void stop();
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] int sampleRate() const noexcept;
    [[nodiscard]] int channels() const noexcept;
    [[nodiscard]] std::string deviceName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vloom
