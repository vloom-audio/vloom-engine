// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/io/AudioDevice.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

namespace vloom {

namespace {
constexpr int kMaxChannels = 8;

void setError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}
} // namespace

struct AudioDevice::Impl {
    ma_context context{};
    bool contextReady = false;

    ma_device device{};
    bool deviceOpen = false;
    std::atomic<bool> running{false};

    std::vector<ma_device_info> playbackInfos;
    Callback callback;
    int channels = 0;

    // Non-interleaved scratch buffers, allocated once in open().
    std::vector<float> scratch;
    std::array<float*, kMaxChannels> channelPtrs{};

    bool ensureContext(std::string* error)
    {
        if (contextReady)
            return true;
        if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
            setError(error, "Could not initialise the audio backend");
            return false;
        }
        contextReady = true;
        return true;
    }

    void refreshDevices()
    {
        playbackInfos.clear();
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&context, &infos, &count, nullptr, nullptr) == MA_SUCCESS)
            playbackInfos.assign(infos, infos + count);
    }

    // Real-time thread.
    static void dataCallback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frameCount)
    {
        auto* self = static_cast<Impl*>(dev->pUserData);
        auto* interleaved = static_cast<float*>(output);
        const int numChannels = self->channels;

        ma_uint32 done = 0;
        while (done < frameCount) {
            const auto block = static_cast<int>(
                std::min<ma_uint32>(frameCount - done, static_cast<ma_uint32>(kMaxBlockFrames)));

            self->callback(self->channelPtrs.data(), numChannels, block);

            float* dst = interleaved + static_cast<std::size_t>(done) * numChannels;
            for (int i = 0; i < block; ++i)
                for (int ch = 0; ch < numChannels; ++ch)
                    dst[i * numChannels + ch] = self->channelPtrs[ch][i];
            done += static_cast<ma_uint32>(block);
        }
    }
};

AudioDevice::AudioDevice()
    : impl_(std::make_unique<Impl>())
{
}

AudioDevice::~AudioDevice()
{
    close();
    if (impl_->contextReady)
        ma_context_uninit(&impl_->context);
}

std::vector<AudioDeviceInfo> AudioDevice::listOutputDevices()
{
    std::vector<AudioDeviceInfo> result;
    if (!impl_->ensureContext(nullptr))
        return result;
    impl_->refreshDevices();
    for (const auto& info : impl_->playbackInfos)
        result.push_back({info.name, info.isDefault != 0});
    return result;
}

bool AudioDevice::open(const AudioDeviceConfig& config, Callback callback, std::string* error)
{
    close();
    if (!callback) {
        setError(error, "No audio callback given");
        return false;
    }
    if (!impl_->ensureContext(error))
        return false;

    const int channels = std::clamp(config.channels, 1, kMaxChannels);

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = static_cast<ma_uint32>(channels);
    dc.sampleRate = static_cast<ma_uint32>(std::max(config.sampleRate, 0));
    dc.periodSizeInFrames = static_cast<ma_uint32>(std::clamp(config.bufferFrames, 16, kMaxBlockFrames));
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.noPreSilencedOutputBuffer = MA_TRUE; // we overwrite every sample
    dc.dataCallback = &Impl::dataCallback;
    dc.pUserData = impl_.get();

    if (config.deviceIndex >= 0) {
        impl_->refreshDevices();
        if (config.deviceIndex >= static_cast<int>(impl_->playbackInfos.size())) {
            setError(error, "Audio device index out of range");
            return false;
        }
        dc.playback.pDeviceID = &impl_->playbackInfos[static_cast<std::size_t>(config.deviceIndex)].id;
    }

    impl_->callback = std::move(callback);
    impl_->channels = channels;
    impl_->scratch.assign(static_cast<std::size_t>(channels) * kMaxBlockFrames, 0.0f);
    for (int ch = 0; ch < channels; ++ch)
        impl_->channelPtrs[ch] = impl_->scratch.data() + static_cast<std::size_t>(ch) * kMaxBlockFrames;

    if (ma_device_init(&impl_->context, &dc, &impl_->device) != MA_SUCCESS) {
        setError(error, "Could not open the audio device");
        impl_->callback = nullptr;
        return false;
    }
    impl_->deviceOpen = true;
    return true;
}

bool AudioDevice::start(std::string* error)
{
    if (!impl_->deviceOpen) {
        setError(error, "Audio device is not open");
        return false;
    }
    if (impl_->running.load())
        return true;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        setError(error, "Could not start the audio device");
        return false;
    }
    impl_->running.store(true);
    return true;
}

void AudioDevice::stop()
{
    if (impl_->deviceOpen && impl_->running.load()) {
        ma_device_stop(&impl_->device);
        impl_->running.store(false);
    }
}

void AudioDevice::close()
{
    if (!impl_->deviceOpen)
        return;
    stop();
    ma_device_uninit(&impl_->device);
    impl_->deviceOpen = false;
    impl_->callback = nullptr;
}

bool AudioDevice::isOpen() const noexcept { return impl_->deviceOpen; }
bool AudioDevice::isRunning() const noexcept { return impl_->running.load(); }

int AudioDevice::sampleRate() const noexcept
{
    return impl_->deviceOpen ? static_cast<int>(impl_->device.sampleRate) : 0;
}

int AudioDevice::channels() const noexcept { return impl_->deviceOpen ? impl_->channels : 0; }

std::string AudioDevice::deviceName() const
{
    if (!impl_->deviceOpen)
        return {};
    char name[MA_MAX_DEVICE_NAME_LENGTH + 1] = {};
    size_t len = 0;
    if (ma_device_get_name(&impl_->device, ma_device_type_playback, name, sizeof(name), &len) != MA_SUCCESS)
        return {};
    return std::string(name, len);
}

} // namespace vloom
