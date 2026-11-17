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
    int deviceIndex = -1; ///< index into listInputDevices(), -1 = system default
    int sampleRate = 48000; ///< 0 = device native rate; use the output's rate
    int bufferFrames = 256; ///< requested period size
    int channels = 2;
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
/// (Engine keeps the difference from growing, ADR 0016).
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vloom
