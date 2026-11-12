// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/io/AudioDevice.hpp"

#include "io/AudioBackend.hpp"

#include <algorithm>
#include <array>
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
    std::unique_ptr<io::AudioBackend> backend = io::createPlatformBackend();
    std::vector<io::BackendDeviceInfo> devices; ///< as last listed, for indices

    bool open = false;
    Callback callback;
    io::BackendStreamInfo info;

    // Non-interleaved scratch buffers, allocated once in open().
    std::vector<float> scratch;
    std::array<float*, kMaxChannels> channelPtrs{};

    // Real-time thread: asks the engine for planar blocks and interleaves them
    // into the backend's buffer.
    static void render(void* user, float* interleaved, int numChannels, int numFrames) noexcept
    {
        auto* self = static_cast<Impl*>(user);
        const int channels = std::min(numChannels, self->info.channels);
        int done = 0;
        while (done < numFrames) {
            const int block = std::min(numFrames - done, kMaxBlockFrames);
            self->callback(self->channelPtrs.data(), channels, block);
            float* dst = interleaved + static_cast<std::size_t>(done) * static_cast<std::size_t>(numChannels);
            for (int i = 0; i < block; ++i) {
                for (int ch = 0; ch < channels; ++ch)
                    dst[i * numChannels + ch] = self->channelPtrs[static_cast<std::size_t>(ch)][i];
                for (int ch = channels; ch < numChannels; ++ch)
                    dst[i * numChannels + ch] = 0.0f;
            }
            done += block;
        }
    }
};

AudioDevice::AudioDevice()
    : impl_(std::make_unique<Impl>())
{
}

AudioDevice::~AudioDevice() { close(); }

std::vector<AudioDeviceInfo> AudioDevice::listOutputDevices()
{
    impl_->devices = impl_->backend->listOutputDevices();
    std::vector<AudioDeviceInfo> result;
    result.reserve(impl_->devices.size());
    for (const auto& device : impl_->devices)
        result.push_back({device.name, device.isDefault});
    return result;
}

bool AudioDevice::open(const AudioDeviceConfig& config, Callback callback, std::string* error)
{
    close();
    if (!callback) {
        setError(error, "No audio callback given");
        return false;
    }

    io::BackendStreamConfig stream;
    stream.sampleRate = std::max(config.sampleRate, 0);
    stream.bufferFrames = std::clamp(config.bufferFrames, 16, kMaxBlockFrames);
    stream.channels = std::clamp(config.channels, 1, kMaxChannels);
    if (config.deviceIndex >= 0) {
        if (impl_->devices.empty())
            impl_->devices = impl_->backend->listOutputDevices();
        if (config.deviceIndex >= static_cast<int>(impl_->devices.size())) {
            setError(error, "Audio device index out of range");
            return false;
        }
        stream.deviceId = impl_->devices[static_cast<std::size_t>(config.deviceIndex)].id;
    }

    impl_->callback = std::move(callback);
    io::BackendStreamInfo info;
    std::string message;
    if (!impl_->backend->open(stream, &Impl::render, impl_.get(), info, message)) {
        setError(error, message.empty() ? "Could not open the audio device" : message);
        impl_->callback = nullptr;
        return false;
    }
    info.channels = std::clamp(info.channels, 1, kMaxChannels);
    impl_->info = info;
    impl_->scratch.assign(static_cast<std::size_t>(info.channels) * kMaxBlockFrames, 0.0f);
    for (int ch = 0; ch < info.channels; ++ch)
        impl_->channelPtrs[static_cast<std::size_t>(ch)]
            = impl_->scratch.data() + static_cast<std::size_t>(ch) * kMaxBlockFrames;
    impl_->open = true;
    return true;
}

bool AudioDevice::start(std::string* error)
{
    if (!impl_->open) {
        setError(error, "Audio device is not open");
        return false;
    }
    if (impl_->backend->isRunning())
        return true;
    std::string message;
    if (!impl_->backend->start(message)) {
        setError(error, message.empty() ? "Could not start the audio device" : message);
        return false;
    }
    return true;
}

void AudioDevice::stop()
{
    if (impl_->open)
        impl_->backend->stop();
}

void AudioDevice::close()
{
    if (!impl_->open)
        return;
    impl_->backend->stop();
    impl_->backend->close();
    impl_->open = false;
    impl_->callback = nullptr;
    impl_->info = {};
}

bool AudioDevice::isOpen() const noexcept { return impl_->open; }
bool AudioDevice::isRunning() const noexcept { return impl_->open && impl_->backend->isRunning(); }
int AudioDevice::sampleRate() const noexcept { return impl_->open ? impl_->info.sampleRate : 0; }
int AudioDevice::channels() const noexcept { return impl_->open ? impl_->info.channels : 0; }
std::string AudioDevice::deviceName() const { return impl_->open ? impl_->info.deviceName : std::string(); }

} // namespace vloom
