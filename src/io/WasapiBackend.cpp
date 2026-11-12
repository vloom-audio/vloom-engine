// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Windows output through WASAPI, shared mode, event driven.
//
// Every COM object of a stream is created and used on one worker thread
// (multi-threaded apartment), so the control thread's apartment (Qt's UI
// thread is single-threaded) never matters. The worker renders on the
// "Pro Audio" MMCSS class. Windows converts the sample rate and channel
// count when the device runs at another format (AUTOCONVERTPCM).
#include "io/AudioBackend.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Defines (selectany) the GUIDs and property keys used below, e.g.
// PKEY_Device_FriendlyName, instead of linking them from a library.
#include <initguid.h>
// Must come before functiondiscoverykeys_devpkey.h, which needs its
// DEFINE_PROPERTYKEY; the comments keep clang-format from reordering them.
#include <mmdeviceapi.h>
// Platform and standard headers.
#include <algorithm>
#include <atomic>
#include <audioclient.h>
#include <avrt.h>
#include <condition_variable>
#include <cstdint>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mutex>
#include <thread>
#include <wrl/client.h>

// COM code uses Microsoft extensions (__uuidof, also inside IID_PPV_ARGS);
// this file only builds on Windows, where they are the normal way.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif

namespace vloom::io {

namespace {

using Microsoft::WRL::ComPtr;

constexpr REFERENCE_TIME kHundredNsPerSecond = 10'000'000;

std::string toUtf8(const wchar_t* text)
{
    if (text == nullptr || *text == L'\0')
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1)
        return {};
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

/// Joins the calling thread to the multi-threaded apartment for its
/// lifetime. A thread already in a single-threaded apartment (the UI
/// thread) keeps it: the device API works from either.
class ComScope {
public:
    ComScope()
        : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }
    ~ComScope()
    {
        if (SUCCEEDED(result_))
            CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool usable() const noexcept { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_;
};

std::string deviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr)
        return {};
    std::string out = toUtf8(id);
    CoTaskMemFree(id);
    return out;
}

std::string friendlyName(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
        name = toUtf8(value.pwszVal);
    PropVariantClear(&value);
    return name;
}

DWORD speakerMask(int channels)
{
    switch (channels) {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default:
        return 0;
    }
}

class WasapiBackend final : public AudioBackend {
public:
    ~WasapiBackend() override { close(); }

    std::vector<BackendDeviceInfo> listOutputDevices() override
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
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice)))
            defaultId = deviceId(defaultDevice.Get());

        ComPtr<IMMDeviceCollection> devices;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
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

    bool open(const BackendStreamConfig& config, RenderFn render, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        config_ = config;
        render_ = render;
        user_ = user;
        quit_.store(false);
        wantRunning_.store(false);
        running_.store(false);
        setupDone_ = false;
        setupOk_ = false;
        error_.clear();

        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake_ == nullptr) {
            error = "Could not create an event for the audio thread";
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
            error = "Audio device is not open";
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
        ComPtr<IAudioRenderClient> renderClient;
        HANDLE audioEvent = nullptr;
        UINT32 bufferFrames = 0;

        const bool ok = com.usable() && setup(client, renderClient, audioEvent, bufferFrames);
        {
            std::lock_guard lock(mutex_);
            if (!com.usable())
                error_ = "Could not initialise COM for the audio thread";
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
                if (want)
                    clientRunning = startClient(client.Get(), renderClient.Get(), bufferFrames);
                else {
                    client->Stop();
                    client->Reset();
                    clientRunning = false;
                    setRunning(false, 0);
                }
            }
            const DWORD waited = WaitForMultipleObjects(2, events, FALSE, clientRunning ? 2000 : INFINITE);
            // An invalidated endpoint (unplugged) may stop signalling the
            // event, so a timeout also checks the device.
            UINT32 padding = 0;
            const bool lost
                = waited == WAIT_TIMEOUT && clientRunning && FAILED(client->GetCurrentPadding(&padding));
            if ((waited == WAIT_OBJECT_0 + 1 && clientRunning) || lost) {
                if (lost || !renderAvailable(client.Get(), renderClient.Get(), bufferFrames)) {
                    // Device lost (unplugged, format changed): stop rendering;
                    // the control thread sees isRunning() == false.
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
        renderClient.Reset();
        client.Reset();
        CloseHandle(audioEvent);
    }

    bool fail(const char* message)
    {
        std::lock_guard lock(mutex_);
        error_ = message;
        return false;
    }

    bool setup(ComPtr<IAudioClient>& client, ComPtr<IAudioRenderClient>& renderClient, HANDLE& audioEvent,
        UINT32& bufferFrames)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
            return fail("Could not list audio devices");

        ComPtr<IMMDevice> device;
        const HRESULT found = config_.deviceId.empty()
            ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
            : enumerator->GetDevice(toWide(config_.deviceId).c_str(), &device);
        if (FAILED(found))
            return fail(
                config_.deviceId.empty() ? "No audio output device" : "The audio device is not available");

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
            return fail("Could not open the audio device");

        int sampleRate = config_.sampleRate;
        if (sampleRate <= 0) {
            WAVEFORMATEX* mix = nullptr;
            if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr)
                return fail("Could not read the device format");
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

        // Requested period, rounded up to whole 100 ns units.
        const auto duration = static_cast<REFERENCE_TIME>(
            (static_cast<std::int64_t>(config_.bufferFrames) * kHundredNsPerSecond + sampleRate - 1)
            / sampleRate);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                reinterpret_cast<const WAVEFORMATEX*>(&format), nullptr)))
            return fail("The audio device does not accept this format");

        if (FAILED(client->GetBufferSize(&bufferFrames)) || bufferFrames == 0)
            return fail("Could not read the device buffer size");
        if (FAILED(client->GetService(IID_PPV_ARGS(&renderClient))))
            return fail("Could not open the device for playback");
        audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (audioEvent == nullptr || FAILED(client->SetEventHandle(audioEvent)))
            return fail("Could not set up the audio event");

        std::lock_guard lock(mutex_);
        info_.sampleRate = sampleRate;
        info_.channels = channels;
        info_.deviceName = friendlyName(device.Get());
        return true;
    }

    bool startClient(IAudioClient* client, IAudioRenderClient* renderClient, UINT32 bufferFrames)
    {
        // Fill the whole buffer first so playback starts without a gap.
        bool ok = renderAvailable(client, renderClient, bufferFrames) && SUCCEEDED(client->Start());
        if (!ok) {
            std::lock_guard lock(mutex_);
            error_ = "Could not start the audio device";
        }
        setRunning(ok, ok ? 1 : -1);
        return ok;
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

    // Real-time: no allocation, no locks.
    bool renderAvailable(IAudioClient* client, IAudioRenderClient* renderClient, UINT32 bufferFrames) noexcept
    {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding)))
            return false;
        const UINT32 frames = bufferFrames - std::min(padding, bufferFrames);
        if (frames == 0)
            return true;
        BYTE* data = nullptr;
        if (FAILED(renderClient->GetBuffer(frames, &data)))
            return false;
        render_(user_, reinterpret_cast<float*>(data), info_.channels, static_cast<int>(frames));
        return SUCCEEDED(renderClient->ReleaseBuffer(frames, 0));
    }

    BackendStreamConfig config_;
    RenderFn render_ = nullptr;
    void* user_ = nullptr;
    BackendStreamInfo info_; ///< written by the worker before setupDone_

    std::thread worker_;
    HANDLE wake_ = nullptr; ///< auto-reset: a control request is waiting
    std::atomic<bool> quit_{false};
    std::atomic<bool> wantRunning_{false};
    std::atomic<bool> running_{false};

    std::mutex mutex_; ///< control-thread handshakes only, never while rendering
    std::condition_variable changed_;
    bool setupDone_ = false;
    bool setupOk_ = false;
    int startResult_ = 0; ///< 1 started, -1 failed, 0 pending
    std::string error_;
};

} // namespace

std::unique_ptr<AudioBackend> createPlatformBackend() { return std::make_unique<WasapiBackend>(); }

} // namespace vloom::io

#endif // _WIN32
