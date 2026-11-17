// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Windows MIDI input through WinMM (midiIn*): every Windows version has it,
// and every class-compliant USB keyboard shows up there. Windows calls back
// on a thread of its own with each short message packed into a word.
#include "io/MidiBackend.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <mmsystem.h>

namespace vloom::io {

namespace {

std::string narrow(const wchar_t* text)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string portName(UINT id)
{
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(id, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return {};
    return narrow(caps.szPname);
}

std::string mmError(const char* what, MMRESULT result)
{
    wchar_t text[MAXERRORLENGTH] = {};
    midiInGetErrorTextW(result, text, MAXERRORLENGTH);
    std::string message = narrow(text);
    return std::string(what) + (message.empty() ? "" : ": " + message);
}

class WinMmMidi final : public MidiBackend {
public:
    explicit WinMmMidi(MidiDeliver deliver)
        : deliver_(std::move(deliver))
    {
    }
    ~WinMmMidi() override { close(); }
    WinMmMidi(const WinMmMidi&) = delete;
    WinMmMidi& operator=(const WinMmMidi&) = delete;

    bool open(int index, std::string* error) override
    {
        close();
        if (index < 0 || static_cast<UINT>(index) >= midiInGetNumDevs()) {
            if (error != nullptr)
                *error = "no MIDI input with that number";
            return false;
        }
        const auto id = static_cast<UINT>(index);
        const MMRESULT result = midiInOpen(&handle_, id, reinterpret_cast<DWORD_PTR>(&WinMmMidi::callback),
            reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            handle_ = nullptr;
            if (error != nullptr)
                *error = mmError("cannot open the MIDI input (another program may be using it)", result);
            return false;
        }
        name_ = portName(id);
        return true;
    }

    bool start(std::string* error) override
    {
        if (handle_ == nullptr)
            return false;
        const MMRESULT result = midiInStart(handle_);
        if (result != MMSYSERR_NOERROR) {
            if (error != nullptr)
                *error = mmError("cannot start the MIDI input", result);
            return false;
        }
        return true;
    }

    void stop() override
    {
        if (handle_ != nullptr)
            midiInStop(handle_);
    }

    void close() override
    {
        if (handle_ == nullptr)
            return;
        midiInStop(handle_);
        midiInReset(handle_);
        midiInClose(handle_); // returns once no callback runs
        handle_ = nullptr;
        name_.clear();
    }

    [[nodiscard]] std::string name() const override { return name_; }

private:
    static void CALLBACK callback(
        HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR /*time*/) noexcept
    {
        if (message != MIM_DATA || instance == 0)
            return;
        auto* self = reinterpret_cast<WinMmMidi*>(instance);
        const MidiMessage midi = unpackMidi(static_cast<unsigned long>(param1));
        if (midi.status == 0)
            return;
        try {
            self->deliver_(midi);
        } catch (...) { // never into Windows' thread
        }
    }

    MidiDeliver deliver_;
    HMIDIIN handle_ = nullptr;
    std::string name_;
};

} // namespace

std::vector<std::string> midiInputNames()
{
    std::vector<std::string> names;
    const UINT count = midiInGetNumDevs();
    for (UINT id = 0; id < count; ++id)
        names.push_back(portName(id));
    return names;
}

std::unique_ptr<MidiBackend> makeMidiBackend(MidiDeliver deliver)
{
    return std::make_unique<WinMmMidi>(std::move(deliver));
}

} // namespace vloom::io

#endif // _WIN32
