// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Linux output through ALSA. PulseAudio and PipeWire are reached through
// ALSA's "default", "pulse" and "pipewire" devices; sound cards through
// "plughw:" devices, which convert the rate and format we ask for. Our own
// thread writes one period at a time with blocking writes.
#include "io/AudioBackend.hpp"

#if defined(__linux__)

#include <algorithm>
#include <alsa/asoundlib.h>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <thread>

namespace vloom::io {

namespace {

/// Hides devices that are aliases or raw hardware ("hw:" fails on most
/// formats; "plughw:" is listed instead).
bool listable(const std::string& name)
{
    if (name == "default" || name == "pulse" || name == "pipewire")
        return true;
    return name.rfind("plughw:", 0) == 0;
}

std::string hintValue(void* hint, const char* id)
{
    char* value = snd_device_name_get_hint(hint, id);
    if (value == nullptr)
        return {};
    std::string out(value);
    std::free(value);
    return out;
}

std::string displayName(const std::string& id, std::string description)
{
    // Descriptions are "Card, Device\nLong text": keep one line.
    std::replace(description.begin(), description.end(), '\n', ' ');
    if (description.empty())
        return id;
    return id == "default" ? description : description + " (" + id + ")";
}

enum class SampleType { Float32, Int32, Int16 };

/// Owns an snd_pcm_hw_params_t (heap, instead of the alloca macro).
struct HwParams {
    snd_pcm_hw_params_t* hw = nullptr;
    HwParams() { snd_pcm_hw_params_malloc(&hw); }
    ~HwParams() { snd_pcm_hw_params_free(hw); }
    HwParams(const HwParams&) = delete;
    HwParams& operator=(const HwParams&) = delete;
};

class AlsaBackend final : public AudioBackend {
public:
    ~AlsaBackend() override { close(); }

    std::vector<BackendDeviceInfo> listOutputDevices() override
    {
        std::vector<BackendDeviceInfo> result;
        void** hints = nullptr;
        if (snd_device_name_hint(-1, "pcm", &hints) < 0 || hints == nullptr)
            return result;
        for (void** hint = hints; *hint != nullptr; ++hint) {
            const std::string name = hintValue(*hint, "NAME");
            const std::string direction = hintValue(*hint, "IOID"); // empty = both
            if (name.empty() || (!direction.empty() && direction != "Output") || !listable(name))
                continue;
            result.push_back({name, displayName(name, hintValue(*hint, "DESC")), name == "default"});
        }
        snd_device_name_free_hint(hints);
        // The default device first, as the other backends do not sort.
        std::stable_partition(result.begin(), result.end(), [](const auto& d) { return d.isDefault; });
        return result;
    }

    bool open(const BackendStreamConfig& config, RenderFn render, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        const std::string name = config.deviceId.empty() ? "default" : config.deviceId;
        if (snd_pcm_open(&pcm_, name.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            pcm_ = nullptr;
            error = "Could not open the audio device '" + name + "'";
            return false;
        }

        HwParams params;
        if (params.hw == nullptr) {
            close();
            error = "Out of memory";
            return false;
        }
        snd_pcm_hw_params_t* hw = params.hw;
        snd_pcm_hw_params_any(pcm_, hw);
        snd_pcm_hw_params_set_rate_resample(pcm_, hw, 1);
        if (snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
            close();
            error = "The audio device does not support interleaved output";
            return false;
        }
        if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_FLOAT_LE) == 0)
            type_ = SampleType::Float32;
        else if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S32_LE) == 0)
            type_ = SampleType::Int32;
        else if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE) == 0)
            type_ = SampleType::Int16;
        else {
            close();
            error = "The audio device supports none of float, 32-bit or 16-bit samples";
            return false;
        }

        unsigned int channels = static_cast<unsigned int>(std::clamp(config.channels, 1, 8));
        if (snd_pcm_hw_params_set_channels_near(pcm_, hw, &channels) < 0) {
            close();
            error = "The audio device does not accept this channel count";
            return false;
        }
        unsigned int rate = config.sampleRate > 0 ? static_cast<unsigned int>(config.sampleRate) : 48000U;
        if (snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr) < 0) {
            close();
            error = "The audio device does not accept this sample rate";
            return false;
        }
        snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(config.bufferFrames);
        snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr);
        unsigned int periods = 3; // enough slack for a busy desktop
        snd_pcm_hw_params_set_periods_near(pcm_, hw, &periods, nullptr);
        if (snd_pcm_hw_params(pcm_, hw) < 0) {
            close();
            error = "The audio device rejected the stream settings";
            return false;
        }
        snd_pcm_hw_params_get_period_size(hw, &period, nullptr);

        render_ = render;
        user_ = user;
        channels_ = static_cast<int>(channels);
        period_ = std::max<snd_pcm_uframes_t>(period, 16);
        // Allocated here, not on the audio thread.
        const std::size_t samples = static_cast<std::size_t>(period_) * channels;
        floatBuffer_.assign(samples, 0.0f);
        int32Buffer_.assign(type_ == SampleType::Int32 ? samples : 0, 0);
        int16Buffer_.assign(type_ == SampleType::Int16 ? samples : 0, 0);

        info.sampleRate = static_cast<int>(rate);
        info.channels = channels_;
        info.deviceName = name;
        return true;
    }

    bool start(std::string& error) override
    {
        if (pcm_ == nullptr) {
            error = "Audio device is not open";
            return false;
        }
        if (running_.load())
            return true;
        if (thread_.joinable())
            thread_.join(); // ended on its own after the device was lost
        if (snd_pcm_prepare(pcm_) < 0) {
            error = "Could not start the audio device";
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() noexcept override
    {
        running_.store(false);
        if (thread_.joinable())
            thread_.join();
        if (pcm_ != nullptr)
            snd_pcm_drop(pcm_);
    }

    void close() noexcept override
    {
        stop();
        if (pcm_ != nullptr) {
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
    }

    [[nodiscard]] bool isRunning() const noexcept override { return running_.load(); }

private:
    void run() noexcept
    {
        // Real-time priority when the user may have it (audio group, rtkit);
        // otherwise keep the normal priority.
        sched_param param{};
        param.sched_priority = std::max(sched_get_priority_min(SCHED_FIFO), 70);
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

        while (running_.load()) {
            const int frames = static_cast<int>(period_);
            render_(user_, floatBuffer_.data(), channels_, frames);
            const void* data = convert(frames);
            snd_pcm_sframes_t left = frames;
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            const std::size_t frameBytes = static_cast<std::size_t>(channels_) * sampleBytes();
            while (left > 0 && running_.load()) {
                snd_pcm_sframes_t written = snd_pcm_writei(pcm_, bytes, static_cast<snd_pcm_uframes_t>(left));
                if (written < 0) {
                    // Underrun or suspend: recover and keep going; give up on
                    // anything else (device unplugged).
                    if (snd_pcm_recover(pcm_, static_cast<int>(written), 1) < 0) {
                        running_.store(false);
                        return;
                    }
                    continue;
                }
                left -= written;
                bytes += static_cast<std::size_t>(written) * frameBytes;
            }
        }
    }

    [[nodiscard]] std::size_t sampleBytes() const noexcept { return type_ == SampleType::Int16 ? 2U : 4U; }

    /// The rendered period in the device's sample format.
    const void* convert(int frames) noexcept
    {
        const std::size_t count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
        if (type_ == SampleType::Float32)
            return floatBuffer_.data();
        if (type_ == SampleType::Int32) {
            std::int32_t* out = int32Buffer_.data();
            for (std::size_t i = 0; i < count; ++i)
                out[i] = static_cast<std::int32_t>(
                    std::lrint(std::clamp(floatBuffer_[i], -1.0f, 1.0f) * 2147483520.0f));
            return out;
        }
        std::int16_t* out = int16Buffer_.data();
        for (std::size_t i = 0; i < count; ++i)
            out[i]
                = static_cast<std::int16_t>(std::lrint(std::clamp(floatBuffer_[i], -1.0f, 1.0f) * 32767.0f));
        return out;
    }

    snd_pcm_t* pcm_ = nullptr;
    SampleType type_ = SampleType::Float32;
    RenderFn render_ = nullptr;
    void* user_ = nullptr;
    int channels_ = 2;
    snd_pcm_uframes_t period_ = 256;
    std::vector<float> floatBuffer_;
    std::vector<std::int32_t> int32Buffer_; ///< used when the device takes 32-bit integers
    std::vector<std::int16_t> int16Buffer_; ///< used when the device takes 16-bit integers
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace

std::unique_ptr<AudioBackend> createPlatformBackend() { return std::make_unique<AlsaBackend>(); }

} // namespace vloom::io

#endif // __linux__
