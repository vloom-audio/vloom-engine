// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/io/AudioDevice.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vloom {

struct AudioInputConfig {
    int deviceIndex = -1; ///< index into listInputDevices(api), -1 = system default
    int sampleRate = 48000; ///< 0 = device native rate; use the output's rate
    int bufferFrames = 256; ///< requested period size
    int channels = 2;
    /// AudioApi::Asio taps the ASIO driver an AudioDevice opened (ADR 0021):
    /// open that output first with AudioDeviceConfig::inputChannels. Its
    /// rate, buffer size and clock are the output's; deviceIndex -1 or the
    /// index of that driver.
    AudioApi api = AudioApi::Default;
};

/// A hardware input device (a microphone, an audio interface's inputs),
/// through each OS's own audio API: WASAPI (shared, event driven) on
/// Windows, CoreAudio (HAL input unit) on macOS, ALSA on Linux. ADR 0016.
///
/// The callback runs on the input's real-time thread with non-interleaved
/// float buffers of at most kMaxBlockFrames frames; Engine::pushAudioInput()
/// is made for it. Where the OS converts (Windows, macOS), the stream runs
/// at the rate asked for; ALSA picks the nearest it can.
///
/// It runs on a clock of its own: the output device's may drift from it
/// (Engine keeps the difference from growing, ADR 0016). An ASIO input is
/// the exception: it shares the output's driver and clock, and its callback
/// runs on the driver's thread just before the output's, every period. It
/// stops running when that output closes; open it again after the output.
class AudioInput {
public:
    /// Runs on the real-time input thread. It should not throw; if it does,
    /// that block is dropped.
    using Callback = std::function<void(const float* const* in, int numChannels, int numFrames)>;

    static constexpr int kMaxBlockFrames = AudioDevice::kMaxBlockFrames;

    AudioInput();
    ~AudioInput();
    AudioInput(const AudioInput&) = delete;
    AudioInput& operator=(const AudioInput&) = delete;

    /// Empty where the OS has no capture backend.
    [[nodiscard]] std::vector<AudioDeviceInfo> listInputDevices();
    /// The inputs of `api`; for ASIO, the installed drivers.
    [[nodiscard]] std::vector<AudioDeviceInfo> listInputDevices(AudioApi api);

    /// Opens (but does not start) the device. On failure returns false and fills `error`.
    bool open(const AudioInputConfig& config, Callback callback, std::string* error = nullptr);
    bool start(std::string* error = nullptr);
    void stop();
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] int sampleRate() const noexcept;
    [[nodiscard]] int channels() const noexcept;
    [[nodiscard]] std::string deviceName() const;
    /// The API of the open stream (Default when closed).
    [[nodiscard]] AudioApi api() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vloom
