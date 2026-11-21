// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/io/AudioInput.hpp"

#include "io/AsioSession.hpp"
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
    std::unique_ptr<io::InputBackend> asio; ///< made on first use of AudioApi::Asio
    /// As last listed, per API, for indices.
    std::array<std::vector<io::BackendDeviceInfo>, 2> devices;

    io::InputBackend* active = nullptr; ///< the open stream's backend
    AudioApi api = AudioApi::Default;
    bool open = false;
    Callback callback;
    io::BackendStreamInfo info;

    // Non-interleaved scratch buffers, allocated once in open().
    std::vector<float> scratch;
    std::array<float*, kMaxChannels> channelPtrs{};

    /// Null where the OS has no capture backend.
    io::InputBackend* backendFor(AudioApi which)
    {
        if (which != AudioApi::Asio)
            return backend.get();
        if (asio == nullptr)
            asio = io::createAsioInputBackend();
        return asio.get();
    }

    static std::size_t index(AudioApi which) noexcept { return which == AudioApi::Asio ? 1 : 0; }

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

std::vector<AudioDeviceInfo> AudioInput::listInputDevices() { return listInputDevices(AudioApi::Default); }

std::vector<AudioDeviceInfo> AudioInput::listInputDevices(AudioApi api)
{
    std::vector<AudioDeviceInfo> result;
    io::InputBackend* backend = impl_->backendFor(api);
    if (backend == nullptr)
        return result;
    auto& devices = impl_->devices[Impl::index(api)];
    devices = backend->listInputDevices();
    result.reserve(devices.size());
    for (const auto& device : devices)
        result.push_back({device.name, device.isDefault});
    return result;
}

bool AudioInput::open(const AudioInputConfig& config, Callback callback, std::string* error)
{
    close();
    io::InputBackend* backend = impl_->backendFor(config.api);
    if (backend == nullptr) {
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
        auto& devices = impl_->devices[Impl::index(config.api)];
        if (devices.empty())
            devices = backend->listInputDevices();
        if (config.deviceIndex >= static_cast<int>(devices.size())) {
            setError(error, "Audio input index out of range");
            return false;
        }
        stream.deviceId = devices[static_cast<std::size_t>(config.deviceIndex)].id;
    }

    impl_->callback = std::move(callback);
    // The scratch buffers are needed as soon as the stream may call back.
    impl_->scratch.assign(static_cast<std::size_t>(kMaxChannels) * kMaxBlockFrames, 0.0f);
    for (int ch = 0; ch < kMaxChannels; ++ch)
        impl_->channelPtrs[static_cast<std::size_t>(ch)]
            = impl_->scratch.data() + static_cast<std::size_t>(ch) * kMaxBlockFrames;
    io::BackendStreamInfo info;
    std::string message;
    if (!backend->open(stream, &Impl::capture, impl_.get(), info, message)) {
        setError(error, message.empty() ? "Could not open the audio input" : message);
        impl_->callback = nullptr;
        return false;
    }
    info.channels = std::clamp(info.channels, 1, kMaxChannels);
    impl_->info = info;
    impl_->active = backend;
    impl_->api = config.api;
    impl_->open = true;
    return true;
}

bool AudioInput::start(std::string* error)
{
    if (!impl_->open) {
        setError(error, "Audio input is not open");
        return false;
    }
    if (impl_->active->isRunning())
        return true;
    std::string message;
    if (!impl_->active->start(message)) {
        setError(error, message.empty() ? "Could not start the audio input" : message);
        return false;
    }
    return true;
}

void AudioInput::stop()
{
    if (impl_->open)
        impl_->active->stop();
}

void AudioInput::close()
{
    if (!impl_->open)
        return;
    impl_->active->stop();
    impl_->active->close();
    impl_->open = false;
    impl_->active = nullptr;
    impl_->api = AudioApi::Default;
    impl_->callback = nullptr;
    impl_->info = {};
}

bool AudioInput::isOpen() const noexcept { return impl_->open; }
bool AudioInput::isRunning() const noexcept { return impl_->open && impl_->active->isRunning(); }
int AudioInput::sampleRate() const noexcept { return impl_->open ? impl_->info.sampleRate : 0; }
int AudioInput::channels() const noexcept { return impl_->open ? impl_->info.channels : 0; }
std::string AudioInput::deviceName() const { return impl_->open ? impl_->info.deviceName : std::string(); }
AudioApi AudioInput::api() const noexcept { return impl_->api; }

} // namespace vloom
