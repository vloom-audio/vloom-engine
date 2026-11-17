// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// Internal: what the WASAPI output and input backends share. Windows only.

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
#include <audioclient.h>
#include <cstdint>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <string>
#include <wrl/client.h>

namespace vloom::io {

using Microsoft::WRL::ComPtr;

inline constexpr REFERENCE_TIME kHundredNsPerSecond = 10'000'000;

inline std::string toUtf8(const wchar_t* text)
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

inline std::wstring toWide(const std::string& text)
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

inline std::string deviceId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr)
        return {};
    std::string out = toUtf8(id);
    CoTaskMemFree(id);
    return out;
}

inline std::string friendlyName(IMMDevice* device)
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

inline DWORD speakerMask(int channels)
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

} // namespace vloom::io

#endif // _WIN32
