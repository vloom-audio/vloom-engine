// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// The real ASIO driver behind AsioSession (ADR 0021), through the host side
// of the Steinberg ASIO SDK 2.3.4 (used under its GPLv3 option; built only
// on Windows with VLOOM_ENGINE_ASIO). The SDK keeps the loaded driver in
// globals (theAsioDriver, asioDrivers) and its callbacks carry no user
// pointer, so one driver per process is a property of the SDK, not only of
// ASIO; AsioSession makes every call from its one driver thread.
#include "io/AsioSession.hpp"

#if defined(_WIN32) && defined(VLOOM_ENGINE_HAS_ASIO)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// The SDK's headers need <windows.h> first, and asiosys.h before asio.h;
// the comments keep clang-format from reordering them.
#include <objbase.h>
// SDK: platform switches.
#include "asiosys.h"
// SDK: the ASIO API.
#include "asio.h"
// SDK: the Windows driver list.
#include "asiodrivers.h"
// Standard headers.
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Defined by the SDK's host/asiodrivers.cpp.
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace vloom::io {

namespace {

/// The session whose buffers the driver calls back into: the SDK's
/// callbacks have no user pointer. Set before ASIOCreateBuffers, cleared
/// (with clearHost()) after ASIODisposeBuffers and at unload.
std::atomic<AsioDriverHost*> g_host{nullptr};
/// Callbacks inside one of the shims below: clearHost() waits for none, so
/// no callback still uses a session that is about to go.
std::atomic<int> g_inFlight{0};

class InFlight {
public:
    InFlight() noexcept { g_inFlight.fetch_add(1); }
    ~InFlight() { g_inFlight.fetch_sub(1); }
    InFlight(const InFlight&) = delete;
    InFlight& operator=(const InFlight&) = delete;
};

void clearHost() noexcept
{
    g_host.store(nullptr);
    while (g_inFlight.load() != 0)
        std::this_thread::yield();
}

void onBufferSwitch(long index, ASIOBool /*directProcess*/)
{
    const InFlight inFlight;
    if (AsioDriverHost* host = g_host.load())
        host->bufferSwitch(static_cast<int>(index));
}

ASIOTime* onBufferSwitchTimeInfo(ASIOTime* /*params*/, long index, ASIOBool /*directProcess*/)
{
    const InFlight inFlight;
    if (AsioDriverHost* host = g_host.load())
        host->bufferSwitch(static_cast<int>(index));
    return nullptr;
}

void onSampleRateDidChange(ASIOSampleRate rate)
{
    const InFlight inFlight;
    if (AsioDriverHost* host = g_host.load())
        host->sampleRateChanged(rate);
}

long onAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/)
{
    const InFlight inFlight;
    if (AsioDriverHost* host = g_host.load())
        return host->message(static_cast<std::int32_t>(selector), static_cast<std::int32_t>(value));
    // Before the session is connected, still answer the questions about the
    // host; nothing is recorded (not accepting).
    AsioMessageState ignored;
    return handleAsioMessage(static_cast<std::int32_t>(selector), static_cast<std::int32_t>(value), ignored);
}

ASIOCallbacks g_callbacks{&onBufferSwitch, &onSampleRateDidChange, &onAsioMessage, &onBufferSwitchTimeInfo};

/// The SDK keeps driver names in the ANSI code page.
std::string ansiToUtf8(const char* text)
{
    const int wideSize = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (wideSize <= 1)
        return {};
    std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), wideSize);
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

std::string utf8ToAnsi(const std::string& text)
{
    const int wideSize = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wideSize <= 1)
        return {};
    std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), wideSize);
    const int size = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

/// The SDK's AsioDrivers reads the registry once, when it is made: a fresh
/// one sees drivers installed since. Only while no driver is loaded.
void refreshDriverList()
{
    delete asioDrivers;
    asioDrivers = new AsioDrivers();
}

class AsioSdkDriver final : public AsioDriver {
public:
    AsioSdkDriver()
        : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
    {
    }

    ~AsioSdkDriver() override
    {
        if (wake_ != nullptr)
            CloseHandle(wake_);
    }

    AsioSdkDriver(const AsioSdkDriver&) = delete;
    AsioSdkDriver& operator=(const AsioSdkDriver&) = delete;

    void threadStarted() override
    {
        // Most drivers expect a single-threaded apartment, and every call
        // to them comes from this thread.
        com_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        window_ = createMessageWindow();
    }

    void threadStopping() override
    {
        unload();
        // AsioDriverList's constructor initialised COM on this thread and
        // its destructor uninitialises it: both here, so they pair.
        delete asioDrivers;
        asioDrivers = nullptr;
        if (window_ != nullptr)
            DestroyWindow(window_);
        window_ = nullptr;
        if (SUCCEEDED(com_))
            CoUninitialize();
    }

    void waitIdle(std::chrono::milliseconds timeout) override
    {
        // Wakes for queued work (wake_) and for any window message, which an
        // apartment must dispatch.
        if (wake_ != nullptr)
            MsgWaitForMultipleObjectsEx(
                1, &wake_, static_cast<DWORD>(timeout.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        else
            MsgWaitForMultipleObjectsEx(
                0, nullptr, static_cast<DWORD>(timeout.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void wakeIdle() override
    {
        if (wake_ != nullptr)
            SetEvent(wake_);
    }

    std::vector<std::string> driverNames() override
    {
        if (!loaded_ || asioDrivers == nullptr)
            refreshDriverList();
        names_.clear();
        std::vector<std::string> names;
        const long count = asioDrivers->asioGetNumDev();
        for (long i = 0; i < count; ++i) {
            // 32 as AsioDrivers::loadDriver() compares them: names of 32
            // bytes or more are cut to 28 and end in "..." there too.
            std::array<char, 32> name{};
            if (asioDrivers->asioGetDriverName(
                    static_cast<int>(i), name.data(), static_cast<int>(name.size()))
                != 0)
                continue;
            // Shown in UTF-8; loaded by the SDK's own bytes, so a name the
            // round trip cannot rebuild exactly still loads.
            names_.emplace_back(ansiToUtf8(name.data()), std::string(name.data()));
            names.push_back(names_.back().first);
        }
        return names;
    }

    std::int32_t load(const std::string& name, std::string& message) override
    {
        unload();
        std::array<char, 64> ansi{};
        std::string converted;
        for (const auto& [utf8, bytes] : names_)
            if (utf8 == name)
                converted = bytes;
        if (converted.empty())
            converted = utf8ToAnsi(name);
        if (converted.empty() || converted.size() >= ansi.size())
            return ASE_NotPresent;
        std::memcpy(ansi.data(), converted.c_str(), converted.size());
        if (!loadAsioDriver(ansi.data()))
            return ASE_NotPresent;

        ASIODriverInfo info{};
        info.asioVersion = asio::kHostAsioVersion;
        // The driver's parent for dialogs and its message target.
        info.sysRef = window_ != nullptr ? window_ : GetDesktopWindow();
        const ASIOError result = ASIOInit(&info);
        if (result != ASE_OK) {
            info.errorMessage[sizeof(info.errorMessage) - 1] = '\0';
            message = ansiToUtf8(info.errorMessage);
            // ASIOInit forgets the driver on failure without releasing it.
            if (asioDrivers != nullptr)
                asioDrivers->removeCurrentDriver();
            return static_cast<std::int32_t>(result);
        }
        loaded_ = true;
        return asio::kOk;
    }

    void unload() noexcept override
    {
        if (!loaded_)
            return;
        ASIOExit();
        clearHost();
        loaded_ = false;
    }

    std::int32_t getChannels(int& inputs, int& outputs) override
    {
        long in = 0;
        long out = 0;
        const ASIOError result = ASIOGetChannels(&in, &out);
        inputs = static_cast<int>(in);
        outputs = static_cast<int>(out);
        return static_cast<std::int32_t>(result);
    }

    std::int32_t getBufferSize(AsioBufferSizes& sizes) override
    {
        long minSize = 0;
        long maxSize = 0;
        long preferred = 0;
        long granularity = 0;
        const ASIOError result = ASIOGetBufferSize(&minSize, &maxSize, &preferred, &granularity);
        sizes.minSize = static_cast<int>(minSize);
        sizes.maxSize = static_cast<int>(maxSize);
        sizes.preferred = static_cast<int>(preferred);
        sizes.granularity = static_cast<int>(granularity);
        return static_cast<std::int32_t>(result);
    }

    std::int32_t canSampleRate(double rate) override
    {
        return static_cast<std::int32_t>(ASIOCanSampleRate(rate));
    }

    std::int32_t getSampleRate(double& rate) override
    {
        ASIOSampleRate current = 0.0;
        const ASIOError result = ASIOGetSampleRate(&current);
        rate = current;
        return static_cast<std::int32_t>(result);
    }

    std::int32_t setSampleRate(double rate) override
    {
        return static_cast<std::int32_t>(ASIOSetSampleRate(rate));
    }

    std::int32_t getChannelType(bool isInput, int channel, std::int32_t& type) override
    {
        ASIOChannelInfo info{};
        info.channel = channel;
        info.isInput = isInput ? ASIOTrue : ASIOFalse;
        const ASIOError result = ASIOGetChannelInfo(&info);
        type = static_cast<std::int32_t>(info.type);
        return static_cast<std::int32_t>(result);
    }

    std::int32_t createBuffers(std::vector<AsioBuffer>& buffers, int frames, AsioDriverHost& host) override
    {
        std::vector<ASIOBufferInfo> infos(buffers.size());
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            infos[i].isInput = buffers[i].isInput ? ASIOTrue : ASIOFalse;
            infos[i].channelNum = buffers[i].channel;
        }
        g_host.store(&host);
        const ASIOError result = ASIOCreateBuffers(
            infos.data(), static_cast<long>(infos.size()), static_cast<long>(frames), &g_callbacks);
        if (result != ASE_OK) {
            clearHost();
            return static_cast<std::int32_t>(result);
        }
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            buffers[i].halves[0] = infos[i].buffers[0];
            buffers[i].halves[1] = infos[i].buffers[1];
        }
        return asio::kOk;
    }

    std::int32_t disposeBuffers() override
    {
        const ASIOError result = ASIODisposeBuffers();
        clearHost();
        return static_cast<std::int32_t>(result);
    }

    std::int32_t getLatencies(int& input, int& output) override
    {
        long in = 0;
        long out = 0;
        const ASIOError result = ASIOGetLatencies(&in, &out);
        input = static_cast<int>(in);
        output = static_cast<int>(out);
        return static_cast<std::int32_t>(result);
    }

    std::int32_t start() override { return static_cast<std::int32_t>(ASIOStart()); }
    std::int32_t stop() override { return static_cast<std::int32_t>(ASIOStop()); }
    std::int32_t controlPanel() override { return static_cast<std::int32_t>(ASIOControlPanel()); }
    std::int32_t outputReady() noexcept override { return static_cast<std::int32_t>(ASIOOutputReady()); }

private:
    /// A hidden message-only window owned by the driver thread: the
    /// driver's sysRef. Null if Windows refuses one.
    static HWND createMessageWindow()
    {
        static const wchar_t* const kClass = L"VloomAudioDriverHost";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        return CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }

    HANDLE wake_ = nullptr; ///< auto-reset: work is queued for the driver thread
    HWND window_ = nullptr;
    HRESULT com_ = E_FAIL;
    bool loaded_ = false;
    std::vector<std::pair<std::string, std::string>> names_; ///< UTF-8 name, the SDK's own bytes
};

} // namespace

std::unique_ptr<AsioDriver> makeAsioDriver() { return std::make_unique<AsioSdkDriver>(); }

} // namespace vloom::io

#endif // _WIN32 && VLOOM_ENGINE_HAS_ASIO
