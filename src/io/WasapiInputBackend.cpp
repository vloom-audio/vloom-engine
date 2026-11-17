// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Windows input through WASAPI, shared mode, event driven: the capture side
// of WasapiBackend. As there, every COM object of a stream lives on one
// worker thread, which runs on the "Pro Audio" MMCSS class, and Windows
// converts the rate and channel count to what was asked (AUTOCONVERTPCM).
#include "io/InputBackend.hpp"

#if defined(_WIN32)

#include "io/WasapiCommon.hpp"
// Platform and standard headers.
#include <algorithm>
#include <atomic>
#include <avrt.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// COM code uses Microsoft extensions (__uuidof, also inside IID_PPV_ARGS).
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif

namespace vloom::io {

namespace {

class WasapiInputBackend final : public InputBackend {
public:
    ~WasapiInputBackend() override { close(); }

    std::vector<BackendDeviceInfo> listInputDevices() override
    {
        std::vector<BackendDeviceInfo> result;
        const ComScope com;
        if (!com.usable())
            return result;
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
            return result;
        std::string defaultId;
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDevice)))
            defaultId = deviceId(defaultDevice.Get());
        ComPtr<IMMDeviceCollection> devices;
        if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices)))
            return result;
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(i, &device)))
                continue;
            BackendDeviceInfo info;
            info.id = deviceId(device.Get());
            info.name = friendlyName(device.Get());
            info.isDefault = !info.id.empty() && info.id == defaultId;
            if (!info.id.empty())
                result.push_back(std::move(info));
        }
        return result;
    }

    bool open(const BackendStreamConfig& config, CaptureFn capture, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        config_ = config;
        capture_ = capture;
        user_ = user;
        quit_.store(false);
        wantRunning_.store(false);
        running_.store(false);
        setupDone_ = false;
        setupOk_ = false;
        error_.clear();

        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake_ == nullptr) {
            error = "Could not create an event for the input thread";
            return false;
        }
        worker_ = std::thread([this] { run(); });

        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return setupDone_; });
        if (!setupOk_) {
            error = error_;
            lock.unlock();
            close();
            return false;
        }
        info = info_;
        return true;
    }

    bool start(std::string& error) override
    {
        if (!worker_.joinable()) {
            error = "Audio input is not open";
            return false;
        }
        std::unique_lock lock(mutex_);
        startResult_ = 0;
        wantRunning_.store(true);
        SetEvent(wake_);
        changed_.wait(lock, [this] { return startResult_ != 0; });
        if (startResult_ < 0) {
            error = error_;
            wantRunning_.store(false);
            return false;
        }
        return true;
    }

    void stop() noexcept override
    {
        if (!worker_.joinable() || !wantRunning_.load())
            return;
        std::unique_lock lock(mutex_);
        wantRunning_.store(false);
        SetEvent(wake_);
        changed_.wait(lock, [this] { return !running_.load(); });
    }

    void close() noexcept override
    {
        if (worker_.joinable()) {
            quit_.store(true);
            SetEvent(wake_);
            worker_.join();
        }
        if (wake_ != nullptr) {
            CloseHandle(wake_);
            wake_ = nullptr;
        }
        running_.store(false);
        wantRunning_.store(false);
    }

    [[nodiscard]] bool isRunning() const noexcept override { return running_.load(); }

private:
    // ---- worker thread -------------------------------------------------

    void run()
    {
        const ComScope com;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> captureClient;
        HANDLE audioEvent = nullptr;

        const bool ok = com.usable() && setup(client, captureClient, audioEvent);
        {
            std::lock_guard lock(mutex_);
            if (!com.usable())
                error_ = "Could not initialise COM for the input thread";
            setupOk_ = ok;
            setupDone_ = true;
        }
        changed_.notify_all();
        if (!ok) {
            if (audioEvent != nullptr)
                CloseHandle(audioEvent);
            return;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        const HANDLE events[2] = {wake_, audioEvent};
        bool clientRunning = false;

        for (;;) {
            if (quit_.load())
                break;
            const bool want = wantRunning_.load();
            if (want != clientRunning) {
                if (want) {
                    clientRunning = SUCCEEDED(client->Start());
                    if (!clientRunning) {
                        std::lock_guard lock(mutex_);
                        error_ = "Could not start the audio input";
                    }
                    setRunning(clientRunning, clientRunning ? 1 : -1);
                } else {
                    client->Stop();
                    client->Reset();
                    clientRunning = false;
                    setRunning(false, 0);
                }
            }
            const DWORD waited = WaitForMultipleObjects(2, events, FALSE, clientRunning ? 2000 : INFINITE);
            // An invalidated endpoint (unplugged) may stop signalling: a
            // timeout checks the device.
            UINT32 padding = 0;
            const bool lost
                = waited == WAIT_TIMEOUT && clientRunning && FAILED(client->GetCurrentPadding(&padding));
            if ((waited == WAIT_OBJECT_0 + 1 && clientRunning) || lost) {
                if (lost || !captureAvailable(captureClient.Get())) {
                    client->Stop();
                    clientRunning = false;
                    wantRunning_.store(false);
                    setRunning(false, 0);
                }
            }
        }

        if (clientRunning)
            client->Stop();
        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics(mmcss);
        captureClient.Reset();
        client.Reset();
        CloseHandle(audioEvent);
    }

    bool fail(const char* message)
    {
        std::lock_guard lock(mutex_);
        error_ = message;
        return false;
    }

    bool setup(ComPtr<IAudioClient>& client, ComPtr<IAudioCaptureClient>& captureClient, HANDLE& audioEvent)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
            return fail("Could not list audio devices");

        ComPtr<IMMDevice> device;
        const HRESULT found = config_.deviceId.empty()
            ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)
            : enumerator->GetDevice(toWide(config_.deviceId).c_str(), &device);
        if (FAILED(found))
            return fail(
                config_.deviceId.empty() ? "No audio input device" : "The audio input is not available");

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
            return fail(
                "Could not open the audio input (is the microphone allowed in the privacy settings?)");

        int sampleRate = config_.sampleRate;
        if (sampleRate <= 0) {
            WAVEFORMATEX* mix = nullptr;
            if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr)
                return fail("Could not read the input format");
            sampleRate = static_cast<int>(mix->nSamplesPerSec);
            CoTaskMemFree(mix);
        }
        const int channels = std::clamp(config_.channels, 1, 8);

        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = static_cast<WORD>(channels);
        format.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = static_cast<WORD>(channels * 4);
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = speakerMask(channels);
        format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        const auto duration = static_cast<REFERENCE_TIME>(
            (static_cast<std::int64_t>(config_.bufferFrames) * kHundredNsPerSecond + sampleRate - 1)
            / sampleRate);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                reinterpret_cast<const WAVEFORMATEX*>(&format), nullptr)))
            return fail("The audio input does not accept this format");

        UINT32 bufferFrames = 0;
        if (FAILED(client->GetBufferSize(&bufferFrames)) || bufferFrames == 0)
            return fail("Could not read the input buffer size");
        if (FAILED(client->GetService(IID_PPV_ARGS(&captureClient))))
            return fail("Could not open the device for recording");
        audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (audioEvent == nullptr || FAILED(client->SetEventHandle(audioEvent)))
            return fail("Could not set up the input event");
        // Silence for packets Windows marks silent (their data may be anything).
        silence_.assign(static_cast<std::size_t>(bufferFrames) * static_cast<std::size_t>(channels), 0.0f);

        std::lock_guard lock(mutex_);
        info_.sampleRate = sampleRate;
        info_.channels = channels;
        info_.deviceName = friendlyName(device.Get());
        return true;
    }

    void setRunning(bool running, int startResult)
    {
        {
            std::lock_guard lock(mutex_);
            running_.store(running);
            if (startResult != 0)
                startResult_ = startResult;
        }
        changed_.notify_all();
    }

    // Real-time: no allocation, no locks. Hands over every packet waiting.
    bool captureAvailable(IAudioCaptureClient* captureClient) noexcept
    {
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packet)))
                return false;
            if (packet == 0)
                return true;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                return false;
            const std::size_t samples
                = static_cast<std::size_t>(frames) * static_cast<std::size_t>(info_.channels);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                // Rarely more than a buffer at once; hand it over in parts.
                for (UINT32 done = 0; done < frames;) {
                    const auto part = static_cast<UINT32>(std::min<std::size_t>(
                        frames - done, silence_.size() / static_cast<std::size_t>(info_.channels)));
                    capture_(user_, silence_.data(), info_.channels, static_cast<int>(part));
                    done += part;
                }
            } else if (samples > 0) {
                capture_(
                    user_, reinterpret_cast<const float*>(data), info_.channels, static_cast<int>(frames));
            }
            if (FAILED(captureClient->ReleaseBuffer(frames)))
                return false;
        }
    }

    BackendStreamConfig config_;
    CaptureFn capture_ = nullptr;
    void* user_ = nullptr;
    BackendStreamInfo info_; ///< written by the worker before setupDone_
    std::vector<float> silence_;

    std::thread worker_;
    HANDLE wake_ = nullptr; ///< auto-reset: a control request is waiting
    std::atomic<bool> quit_{false};
    std::atomic<bool> wantRunning_{false};
    std::atomic<bool> running_{false};

    std::mutex mutex_; ///< control-thread handshakes only, never while capturing
    std::condition_variable changed_;
    bool setupDone_ = false;
    bool setupOk_ = false;
    int startResult_ = 0; ///< 1 started, -1 failed, 0 pending
    std::string error_;
};

} // namespace

std::unique_ptr<InputBackend> createPlatformInputBackend() { return std::make_unique<WasapiInputBackend>(); }

} // namespace vloom::io

#endif // _WIN32
