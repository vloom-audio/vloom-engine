// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vloom {

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
};

/// Which driver API a device is opened through.
enum class AudioApi : std::uint8_t {
    Default, ///< the OS's own: WASAPI on Windows, CoreAudio on macOS, ALSA on Linux
    /// Windows: an ASIO driver (ADR 0021), in builds with VLOOM_ENGINE_ASIO.
    /// Elsewhere it lists nothing and fails to open; see isApiAvailable().
    Asio,
};

struct AudioDeviceConfig {
    int deviceIndex = -1; ///< index into listOutputDevices(api), -1 = system default (ASIO: the first driver)
    int sampleRate = 48000; ///< 0 = device native rate
    int bufferFrames = 256; ///< requested period size
    int channels = 2;
    bool exclusive = false; ///< Windows: WASAPI exclusive mode; ignored elsewhere and by ASIO
    AudioApi api = AudioApi::Default;
    /// ASIO only: driver inputs, from the first, opened with the output for
    /// an AudioInput to tap (AudioInputConfig::api). 0 = output only.
    int inputChannels = 0;
};

/// A hardware output device, through each OS's own audio API: WASAPI
/// (shared or exclusive, event driven) on Windows, CoreAudio (HAL output
/// unit) on macOS, ALSA on Linux (which also reaches PulseAudio and PipeWire).
///
/// The callback runs on the real-time audio thread and receives
/// non-interleaved float buffers of at most kMaxBlockFrames frames. When the
/// device runs at another rate or channel count the OS converts, so
/// sampleRate() reports the rate the callback is driven at.
///
/// Exclusive mode (Windows, AudioDeviceConfig::exclusive, ADR 0020) bypasses
/// the Windows mixer: no rate or channel conversion and no limiter, so the
/// device must accept the requested rate (0 = its mix format rate) and
/// channel count as they are. The engine's float output is written as the
/// first format the device takes of float32, int32, int24 in 32 bits, packed
/// int24 and int16; samples beyond [-1, 1] hard-clip. If the device is busy,
/// forbids exclusive use or takes none of these formats, open() fails with a
/// message; it never falls back to shared mode. isExclusive() tells whether
/// the open stream runs exclusive.
///
/// ASIO (Windows, AudioDeviceConfig::api = AudioApi::Asio, ADR 0021) opens
/// one of the installed ASIO drivers; only one can be loaded per process.
/// The driver runs at the requested rate (or fails with a message naming
/// the driver and the ASIO error) and at the requested buffer size when it
/// allows that size, else at its preferred one (bufferFrames()). Samples are
/// converted to the driver's format and clipped to [-1, 1]. The driver may
/// ask for the stream to be reopened (resetRequested()); the control thread
/// should then close() and open() again. Its inputs reach an AudioInput
/// opened with AudioApi::Asio, on the same clock (inputChannels).
///
/// If the device is lost while running (unplugged), isRunning() turns false.
class AudioDevice {
public:
    /// Runs on the real-time audio thread. It should not throw; if it does,
    /// that block is silent.
    using Callback = std::function<void(float* const* out, int numChannels, int numFrames)>;

    /// Largest block the callback will ever receive.
    static constexpr int kMaxBlockFrames = 4096;

    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    /// The default API's outputs (listOutputDevices(AudioApi::Default)).
    [[nodiscard]] std::vector<AudioDeviceInfo> listOutputDevices();
    /// The outputs of `api`; for ASIO, the installed drivers. Empty where
    /// the API is not available.
    [[nodiscard]] std::vector<AudioDeviceInfo> listOutputDevices(AudioApi api);

    /// Whether `api` can be used in this build on this platform.
    [[nodiscard]] static bool isApiAvailable(AudioApi api) noexcept;

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
    /// True only while open and the stream really runs in WASAPI exclusive mode.
    [[nodiscard]] bool isExclusive() const noexcept;
    /// The API of the open stream (Default when closed).
    [[nodiscard]] AudioApi api() const noexcept;
    /// The period the open stream runs at; 0 when the API does not say
    /// (today only ASIO does).
    [[nodiscard]] int bufferFrames() const noexcept;
    /// Latency the driver reports for its input and its output, in frames;
    /// 0 when unknown (today only ASIO reports it). Their sum is the round
    /// trip for Engine::setAudioRecordLatency().
    [[nodiscard]] int inputLatencyFrames() const noexcept;
    [[nodiscard]] int outputLatencyFrames() const noexcept;
    /// True once the driver asked for the open stream to be reopened (ASIO:
    /// reset request, buffer size, resync or latency change, or a sample
    /// rate change). Poll from the control thread, then close() and open().
    [[nodiscard]] bool resetRequested() const noexcept;

    /// Opens the driver's own settings dialog (ASIO: ASIOControlPanel), for
    /// an open stream. Other APIs have none: returns false with a message.
    /// The driver may then ask for a reset (resetRequested()).
    bool showControlPanel(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Counts changes to the list of output devices as the OS reports them
/// (outputs plugged in or out, the default output changed), so a caller can
/// list devices again only when something changed. Listing them costs
/// 50-200 ms on Windows and disturbs other threads: polling it every couple of
/// seconds made the UI stutter.
///
/// Threading: create and destroy on one thread (on Windows it keeps COM
/// initialised there meanwhile); generation() from any thread.
class DeviceListWatcher {
public:
    DeviceListWatcher();
    ~DeviceListWatcher();
    DeviceListWatcher(const DeviceListWatcher&) = delete;
    DeviceListWatcher& operator=(const DeviceListWatcher&) = delete;

    /// False where the OS cannot notify (ALSA) or refused: list devices
    /// again from time to time instead.
    [[nodiscard]] bool isSupported() const noexcept;

    /// Starts at 0 and grows after each change. One change may count more
    /// than once; compare with the value at the last listing.
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vloom
