// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Linux input through ALSA: the capture side of AlsaBackend. The same
// devices ("default", "pulse", "pipewire", "plughw:"), our own thread
// reading one period at a time with blocking reads.
#include "io/InputBackend.hpp"

#if defined(__linux__)

#include <algorithm>
#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <thread>

namespace vloom::io {

namespace {

bool listable(const std::string& name)
{
    if (name == "default" || name == "pulse" || name == "pipewire")
        return true;
    return name.rfind("plughw:", 0) == 0;
}

std::string hint(void* entry, const char* id)
{
    char* value = snd_device_name_get_hint(entry, id);
    if (value == nullptr)
        return {};
    std::string out(value);
    std::free(value);
    return out;
}

enum class SampleType { Float32, Int32, Int16 };

class AlsaInputBackend final : public InputBackend {
public:
    ~AlsaInputBackend() override { close(); }

    std::vector<BackendDeviceInfo> listInputDevices() override
    {
        std::vector<BackendDeviceInfo> result;
        void** hints = nullptr;
        if (snd_device_name_hint(-1, "pcm", &hints) < 0 || hints == nullptr)
            return result;
        for (void** entry = hints; *entry != nullptr; ++entry) {
            const std::string name = hint(*entry, "NAME");
            const std::string direction = hint(*entry, "IOID"); // empty = both
            if (name.empty() || (!direction.empty() && direction != "Input") || !listable(name))
                continue;
            std::string description = hint(*entry, "DESC");
            std::replace(description.begin(), description.end(), '\n', ' ');
            const std::string shown = description.empty() ? name
                : name == "default"                       ? description
                                                          : description + " (" + name + ")";
            result.push_back({name, shown, name == "default"});
        }
        snd_device_name_free_hint(hints);
        std::stable_partition(result.begin(), result.end(), [](const auto& d) { return d.isDefault; });
        return result;
    }

    bool open(const BackendStreamConfig& config, CaptureFn capture, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        const std::string name = config.deviceId.empty() ? "default" : config.deviceId;
        if (snd_pcm_open(&pcm_, name.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
            pcm_ = nullptr;
            error = "Could not open the audio input '" + name + "'";
            return false;
        }
        const auto fail = [&](const std::string& message) {
            error = message; // before close(), which may clear error_
            close();
            return false;
        };
        snd_pcm_hw_params_t* hw = nullptr;
        if (snd_pcm_hw_params_malloc(&hw) < 0 || hw == nullptr)
            return fail("Out of memory");
        const bool ok = configure(config, hw);
        unsigned int channels = 0;
        unsigned int rate = 0;
        snd_pcm_uframes_t period = 0;
        if (ok) {
            snd_pcm_hw_params_get_channels(hw, &channels);
            snd_pcm_hw_params_get_rate(hw, &rate, nullptr);
            snd_pcm_hw_params_get_period_size(hw, &period, nullptr);
        }
        snd_pcm_hw_params_free(hw);
        if (!ok)
            return fail(error_);

        capture_ = capture;
        user_ = user;
        channels_ = static_cast<int>(channels);
        period_ = std::max<snd_pcm_uframes_t>(period, 16);
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
            error = "Audio input is not open";
            return false;
        }
        if (running_.load())
            return true;
        if (thread_.joinable())
            thread_.join(); // ended on its own after the device was lost
        if (snd_pcm_prepare(pcm_) < 0 || snd_pcm_start(pcm_) < 0) {
            error = "Could not start the audio input";
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() noexcept override
    {
        running_.store(false);
        if (pcm_ != nullptr)
            snd_pcm_drop(pcm_); // wakes a blocking read
        if (thread_.joinable())
            thread_.join();
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
    bool configure(const BackendStreamConfig& config, snd_pcm_hw_params_t* hw)
    {
        snd_pcm_hw_params_any(pcm_, hw);
        snd_pcm_hw_params_set_rate_resample(pcm_, hw, 1);
        if (snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
            error_ = "The audio input does not support interleaved samples";
            return false;
        }
        if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_FLOAT_LE) == 0)
            type_ = SampleType::Float32;
        else if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S32_LE) == 0)
            type_ = SampleType::Int32;
        else if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE) == 0)
            type_ = SampleType::Int16;
        else {
            error_ = "The audio input supports none of float, 32-bit or 16-bit samples";
            return false;
        }
        unsigned int channels = static_cast<unsigned int>(std::clamp(config.channels, 1, 8));
        if (snd_pcm_hw_params_set_channels_near(pcm_, hw, &channels) < 0) {
            error_ = "The audio input does not accept this channel count";
            return false;
        }
        unsigned int rate = config.sampleRate > 0 ? static_cast<unsigned int>(config.sampleRate) : 48000U;
        if (snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr) < 0) {
            error_ = "The audio input does not accept this sample rate";
            return false;
        }
        snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(config.bufferFrames);
        snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr);
        unsigned int periods = 4;
        snd_pcm_hw_params_set_periods_near(pcm_, hw, &periods, nullptr);
        if (snd_pcm_hw_params(pcm_, hw) < 0) {
            error_ = "The audio input rejected the stream settings";
            return false;
        }
        return true;
    }

    void run() noexcept
    {
        sched_param param{};
        param.sched_priority = std::max(sched_get_priority_min(SCHED_FIFO), 70);
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

        void* target = type_ == SampleType::Float32 ? static_cast<void*>(floatBuffer_.data())
            : type_ == SampleType::Int32            ? static_cast<void*>(int32Buffer_.data())
                                                    : static_cast<void*>(int16Buffer_.data());
        while (running_.load()) {
            const snd_pcm_sframes_t got = snd_pcm_readi(pcm_, target, period_);
            if (got < 0) {
                if (!running_.load())
                    return;
                // Overrun or suspend: recover and keep going; give up on
                // anything else (device unplugged).
                if (snd_pcm_recover(pcm_, static_cast<int>(got), 1) < 0) {
                    running_.store(false);
                    return;
                }
                snd_pcm_start(pcm_);
                continue;
            }
            const int frames = static_cast<int>(got);
            convert(frames);
            capture_(user_, floatBuffer_.data(), channels_, frames);
        }
    }

    /// What was read, as floats in floatBuffer_.
    void convert(int frames) noexcept
    {
        const std::size_t count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
        if (type_ == SampleType::Int32)
            for (std::size_t i = 0; i < count; ++i)
                floatBuffer_[i] = static_cast<float>(static_cast<double>(int32Buffer_[i]) / 2147483648.0);
        else if (type_ == SampleType::Int16)
            for (std::size_t i = 0; i < count; ++i)
                floatBuffer_[i] = static_cast<float>(int16Buffer_[i]) / 32768.0f;
    }

    snd_pcm_t* pcm_ = nullptr;
    SampleType type_ = SampleType::Float32;
    std::string error_;
    CaptureFn capture_ = nullptr;
    void* user_ = nullptr;
    int channels_ = 2;
    snd_pcm_uframes_t period_ = 256;
    std::vector<float> floatBuffer_;
    std::vector<std::int32_t> int32Buffer_;
    std::vector<std::int16_t> int16Buffer_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace

std::unique_ptr<InputBackend> createPlatformInputBackend() { return std::make_unique<AlsaInputBackend>(); }

} // namespace vloom::io

#endif // __linux__
