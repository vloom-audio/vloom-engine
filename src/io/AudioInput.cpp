// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/io/AudioInput.hpp"

#include "io/InputBackend.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace vloom {

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
namespace io {
std::unique_ptr<InputBackend> createPlatformInputBackend() { return nullptr; }
} // namespace io
#endif

namespace {
constexpr int kMaxChannels = 8;

void setError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}
} // namespace

struct AudioInput::Impl {
    std::unique_ptr<io::InputBackend> backend = io::createPlatformInputBackend();
    std::vector<io::BackendDeviceInfo> devices; ///< as last listed, for indices

    bool open = false;
    Callback callback;
    io::BackendStreamInfo info;

    // Non-interleaved scratch buffers, allocated once in open().
    std::vector<float> scratch;
    std::array<float*, kMaxChannels> channelPtrs{};

    // Real-time thread: splits the backend's interleaved frames into planar
    // blocks for the callback.
    static void capture(void* user, const float* interleaved, int numChannels, int numFrames) noexcept
    {
        auto* self = static_cast<Impl*>(user);
        const int channels = std::min(numChannels, self->info.channels);
        if (interleaved == nullptr || channels <= 0)
            return;
        int done = 0;
        while (done < numFrames) {
            const int block = std::min(numFrames - done, kMaxBlockFrames);
            const float* src
                = interleaved + static_cast<std::size_t>(done) * static_cast<std::size_t>(numChannels);
            for (int i = 0; i < block; ++i)
                for (int ch = 0; ch < channels; ++ch)
                    self->channelPtrs[static_cast<std::size_t>(ch)][i] = src[i * numChannels + ch];
            try {
                self->callback(self->channelPtrs.data(), channels, block);
            } catch (...) {
                // Never into the OS's audio thread: this block is dropped.
            }
            done += block;
        }
    }
};

AudioInput::AudioInput()
    : impl_(std::make_unique<Impl>())
{
}

AudioInput::~AudioInput() { close(); }

std::vector<AudioDeviceInfo> AudioInput::listInputDevices()
{
    std::vector<AudioDeviceInfo> result;
    if (impl_->backend == nullptr)
        return result;
    impl_->devices = impl_->backend->listInputDevices();
    result.reserve(impl_->devices.size());
    for (const auto& device : impl_->devices)
        result.push_back({device.name, device.isDefault});
    return result;
}

bool AudioInput::open(const AudioInputConfig& config, Callback callback, std::string* error)
{
    close();
    if (impl_->backend == nullptr) {
        setError(error, "Audio input is not supported on this system");
        return false;
    }
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
            impl_->devices = impl_->backend->listInputDevices();
        if (config.deviceIndex >= static_cast<int>(impl_->devices.size())) {
            setError(error, "Audio input index out of range");
            return false;
        }
        stream.deviceId = impl_->devices[static_cast<std::size_t>(config.deviceIndex)].id;
    }

    impl_->callback = std::move(callback);
    // The scratch buffers are needed as soon as the stream may call back.
    impl_->scratch.assign(static_cast<std::size_t>(kMaxChannels) * kMaxBlockFrames, 0.0f);
    for (int ch = 0; ch < kMaxChannels; ++ch)
        impl_->channelPtrs[static_cast<std::size_t>(ch)]
            = impl_->scratch.data() + static_cast<std::size_t>(ch) * kMaxBlockFrames;
    io::BackendStreamInfo info;
    std::string message;
    if (!impl_->backend->open(stream, &Impl::capture, impl_.get(), info, message)) {
        setError(error, message.empty() ? "Could not open the audio input" : message);
        impl_->callback = nullptr;
        return false;
    }
    info.channels = std::clamp(info.channels, 1, kMaxChannels);
    impl_->info = info;
    impl_->open = true;
    return true;
}

bool AudioInput::start(std::string* error)
{
    if (!impl_->open) {
        setError(error, "Audio input is not open");
        return false;
    }
    if (impl_->backend->isRunning())
        return true;
    std::string message;
    if (!impl_->backend->start(message)) {
        setError(error, message.empty() ? "Could not start the audio input" : message);
        return false;
    }
    return true;
}

void AudioInput::stop()
{
    if (impl_->open)
        impl_->backend->stop();
}

void AudioInput::close()
{
    if (!impl_->open)
        return;
    impl_->backend->stop();
    impl_->backend->close();
    impl_->open = false;
    impl_->callback = nullptr;
    impl_->info = {};
}

bool AudioInput::isOpen() const noexcept { return impl_->open; }
bool AudioInput::isRunning() const noexcept { return impl_->open && impl_->backend->isRunning(); }
int AudioInput::sampleRate() const noexcept { return impl_->open ? impl_->info.sampleRate : 0; }
int AudioInput::channels() const noexcept { return impl_->open ? impl_->info.channels : 0; }
std::string AudioInput::deviceName() const { return impl_->open ? impl_->info.deviceName : std::string(); }

} // namespace vloom
