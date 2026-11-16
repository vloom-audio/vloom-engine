// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// vloom-plugin-bridge: loads ONE VST 2 plugin and runs it for an engine in
// another process (ADR 0011). Built for 32 bits it lets a 64-bit host use
// 32-bit plugins; built like the host it keeps a plugin's crash out of the
// host. The engine starts it; it is of no use by hand.
//
//   vloom-plugin-bridge <memory> <control request> <control answer>
//                       <audio request> <audio answer> <host process>
//
// Six handles the host let it inherit, as decimal numbers
// (plugin/bridge/BridgeProtocol.hpp). Three threads:
//
// - the main thread owns the plugin's editor and answers the host's control
//   requests. A request arrives as a message to a hidden window, so it is
//   answered also while the plugin runs a message loop of its own (a
//   dialog, a menu);
// - the audio thread processes blocks;
// - the watch thread turns the host's "a request is there" into that
//   message, and ends the process at once when the host is gone.
//
// Windows only: there the 32-bit plugins are.

#include "plugin/Vst2Abi.hpp"
#include "plugin/bridge/BridgeProtocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h:
#include <avrt.h>
#include <ole2.h>
#include <xmmintrin.h>

namespace {

using namespace vloom;

constexpr int kMaxChannels = 256;
constexpr UINT kRequestMessage = WM_APP + 1;
constexpr UINT_PTR kIdleTimer = 1;

bridge::Shared* shared = nullptr;
HANDLE controlAnswer = nullptr;
bool quitAsked = false; ///< the host said quit: the next WM_QUIT is ours

HMODULE module = nullptr;
vst2::Effect* effect = nullptr;
bool active = false;
bool editorOpen = false;
double sampleRate = 48000.0;
int blockFrames = 0;
std::atomic<DWORD> audioThreadId{0};
vst2::TimeInfo timeInfo{};
SRWLOCK noticeLock = SRWLOCK_INIT;
/// Audio thread: the notes the plugin sent while it processed the block.
std::array<bridge::MidiMessage, bridge::kMaxMadeNotes> sentNotes{};
std::int32_t madeCount = 0;
bool collecting = false;
HANDLE lastBlob = nullptr; ///< the blob of the last answer, kept until the next request

std::vector<std::vector<float>> inputs, outputs;
std::vector<float*> inputPointers, outputPointers;

std::array<vst2::MidiEvent, bridge::kMaxEvents> midi{};
struct EventBlock {
    std::int32_t numEvents = 0;
    std::intptr_t reserved = 0;
    std::array<vst2::MidiEvent*, bridge::kMaxEvents> pointers{};
} events;

std::intptr_t dispatch(std::int32_t opcode, std::int32_t index = 0, std::intptr_t value = 0,
    void* ptr = nullptr, float opt = 0.0f)
{
    return effect->dispatcher(effect, opcode, index, value, ptr, opt);
}

void copyText(char (&to)[bridge::kTextSize], const char* from)
{
    std::size_t i = 0;
    for (; from != nullptr && from[i] != '\0' && i + 1 < static_cast<std::size_t>(bridge::kTextSize); ++i)
        to[i] = from[i];
    to[i] = '\0';
}

/// A string the plugin writes for `opcode` (they write more than the 8 to
/// 64 characters the interface names: the buffer has room).
void pluginText(char (&to)[bridge::kTextSize], std::int32_t opcode, std::int32_t index = 0)
{
    std::array<char, 1024> buffer{};
    dispatch(opcode, index, 0, buffer.data());
    buffer.back() = '\0';
    copyText(to, buffer.data());
}

void notify(std::int32_t kind, std::int32_t index, float value, std::int32_t size)
{
    AcquireSRWLockExclusive(&noticeLock);
    bridge::Notices& notices = shared->notices;
    const std::int32_t written = notices.written;
    if (static_cast<std::uint32_t>(written - notices.read)
        < static_cast<std::uint32_t>(bridge::kMaxNotices)) {
        notices.items[static_cast<std::uint32_t>(written) % bridge::kMaxNotices] = {kind, index, value, size};
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&notices.written), written + 1);
    } // full: dropped
    ReleaseSRWLockExclusive(&noticeLock);
}

void hostText(void* ptr, const char* text)
{
    if (ptr != nullptr)
        std::memcpy(ptr, text, std::strlen(text) + 1); // room for 64 characters, by the interface
}

std::intptr_t VLOOM_VST2_CALL hostCallback(
    vst2::Effect*, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt)
{
    switch (opcode) {
    case vst2::kHostVersion:
        return 2400;
    case vst2::kHostAutomate:
        if (shared != nullptr && index >= 0 && std::isfinite(opt))
            notify(1, index, std::clamp(opt, 0.0f, 1.0f), 0);
        return 0;
    case vst2::kHostProcessEvents: {
        // Notes from the plugin, taken only while it processes a block.
        const auto* sent = static_cast<const vst2::Events*>(ptr);
        if (sent == nullptr || GetCurrentThreadId() != audioThreadId.load(std::memory_order_relaxed)
            || !collecting)
            return 0;
        const auto* list = reinterpret_cast<vst2::MidiEvent* const*>(
            reinterpret_cast<const char*>(sent) + offsetof(vst2::Events, events));
        const std::int32_t count = std::clamp(sent->numEvents, 0, 1024);
        for (std::int32_t i = 0; i < count && madeCount < bridge::kMaxMadeNotes; ++i) {
            const vst2::MidiEvent* e = list[i];
            if (e == nullptr || e->type != vst2::kMidiEventType)
                continue;
            const int status = static_cast<unsigned char>(e->midiData[0]) & 0xF0;
            if (status != 0x90 && status != 0x80)
                continue;
            bridge::MidiMessage& m = sentNotes[static_cast<std::size_t>(madeCount++)];
            m.frame = e->deltaFrames;
            std::memcpy(m.data, e->midiData, 4);
        }
        return 1;
    }
    case vst2::kHostGetTime:
        return reinterpret_cast<std::intptr_t>(&timeInfo);
    case vst2::kHostSizeWindow:
        if (!editorOpen || shared == nullptr)
            return 0;
        notify(2, std::max(index, 0), 0.0f,
            static_cast<std::int32_t>(std::clamp<std::intptr_t>(value, 0, 1 << 20)));
        return 1;
    case vst2::kHostGetSampleRate:
        return static_cast<std::intptr_t>(sampleRate);
    case vst2::kHostGetBlockSize:
        return blockFrames;
    case vst2::kHostGetCurrentProcessLevel:
        return GetCurrentThreadId() == audioThreadId.load(std::memory_order_relaxed) ? 2 : 1;
    case vst2::kHostGetVendorString:
        hostText(ptr, "Vloom Audio");
        return 1;
    case vst2::kHostGetProductString:
        hostText(ptr, "Vloom");
        return 1;
    case vst2::kHostGetVendorVersion:
        return 1000;
    case vst2::kHostCanDo: {
        static constexpr const char* kCan[] = {"sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo",
            "receiveVstEvents", "receiveVstMidiEvent", "sizeWindow"};
        const char* asked = static_cast<const char*>(ptr);
        if (asked == nullptr)
            return 0;
        for (const char* can : kCan)
            if (std::strncmp(asked, can, 64) == 0)
                return 1;
        return 0;
    }
    case vst2::kHostGetLanguage:
        return 1; // English
    case vst2::kHostBeginEdit:
    case vst2::kHostEndEdit:
        return 1;
    default:
        return 0;
    }
}

std::wstring wide(const char* utf8)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), length);
    out.resize(static_cast<std::size_t>(length) - 1);
    return out;
}

template <typename Function> Function functionAt(FARPROC symbol) noexcept
{
    static_assert(sizeof(Function) == sizeof(FARPROC));
    Function f = nullptr;
    std::memcpy(&f, &symbol, sizeof(f));
    return f;
}

/// A handle from its number (the host's on the command line or in a request).
HANDLE handleOf(std::uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)); }

/// Hands `size` bytes at `data` to the host as a blob; false if it cannot.
bool sendBlob(bridge::Control& control, const void* data, std::int64_t size)
{
    if (data == nullptr || size <= 0 || size > bridge::kMaxBlobBytes)
        return false;
    const auto bytes = static_cast<std::uint64_t>(size);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xFFFFFFFFu), nullptr);
    if (mapping == nullptr)
        return false;
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(size));
    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }
    std::memcpy(view, data, static_cast<std::size_t>(size));
    UnmapViewOfFile(view);
    lastBlob = mapping; // the host copies the handle before it asks for anything else
    control.blob = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(mapping));
    control.value = size;
    return true;
}

bool load(bridge::Control& control)
{
    control.text[bridge::kTextSize - 1] = '\0';
    const std::wstring file = wide(control.text);
    // The plugin's own folder first for the DLLs it brings along.
    module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        copyText(
            control.text, ("cannot load the file (error " + std::to_string(GetLastError()) + ")").c_str());
        return false;
    }
    vst2::EntryPoint entry = nullptr;
    for (const char* name : {"VSTPluginMain", "main"}) {
        entry = functionAt<vst2::EntryPoint>(GetProcAddress(module, name));
        if (entry != nullptr)
            break;
    }
    if (entry == nullptr) {
        copyText(control.text, "not a VST 2 plugin (no VSTPluginMain)");
        return false;
    }
    vst2::Effect* made = entry(&hostCallback);
    if (made == nullptr || made->magic != vst2::kMagic || made->dispatcher == nullptr) {
        copyText(control.text, "the plugin could not be created");
        return false;
    }
    effect = made;
    dispatch(vst2::kOpen);
    if (dispatch(vst2::kGetPlugCategory) == vst2::kCategoryShell) {
        copyText(control.text, "the file is a shell of many plugins, which is not supported");
        return false;
    }
    if ((effect->flags & vst2::kCanReplacing) == 0 || effect->processReplacing == nullptr
        || effect->setParameter == nullptr || effect->getParameter == nullptr) {
        copyText(control.text, "the plugin cannot process 32-bit audio");
        return false;
    }
    bridge::PluginFacts& facts = shared->plugin;
    facts.parameters = effect->numParams;
    facts.inputs = effect->numInputs;
    facts.outputs = effect->numOutputs;
    facts.flags = effect->flags;
    facts.uniqueId = effect->uniqueId;
    facts.version = effect->version;
    facts.latency = effect->initialDelay;
    facts.programs = effect->numPrograms;
    char sends[] = "sendVstMidiEvent";
    facts.makesNotes = dispatch(vst2::kCanDo, 0, 0, sends) == 1 ? 1 : 0;
    pluginText(facts.name, vst2::kGetEffectName);
    if (facts.name[0] == '\0')
        pluginText(facts.name, vst2::kGetProductString);
    pluginText(facts.vendor, vst2::kGetVendorString);
    return true;
}

bool activate(double rate, int frames)
{
    const int ins = effect->numInputs;
    const int outs = effect->numOutputs;
    if (ins < 0 || outs < 0 || ins > kMaxChannels || outs > kMaxChannels || frames <= 0
        || frames > bridge::kMaxFrames || !std::isfinite(rate) || rate <= 0.0)
        return false;
    const auto size
        = [frames](int count, std::vector<std::vector<float>>& channels, std::vector<float*>& pointers) {
              channels.assign(
                  static_cast<std::size_t>(count), std::vector<float>(static_cast<std::size_t>(frames)));
              pointers.clear();
              for (auto& channel : channels)
                  pointers.push_back(channel.data());
          };
    size(ins, inputs, inputPointers);
    size(outs, outputs, outputPointers);
    sampleRate = rate;
    blockFrames = frames;
    dispatch(vst2::kSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate));
    dispatch(vst2::kSetBlockSize, 0, frames);
    dispatch(vst2::kMainsChanged, 0, 1);
    dispatch(vst2::kStartProcess);
    active = true;
    return true;
}

void deactivate()
{
    if (!active)
        return;
    dispatch(vst2::kStopProcess);
    dispatch(vst2::kMainsChanged, 0, 0);
    active = false;
}

void closeEditor()
{
    if (!editorOpen)
        return;
    dispatch(vst2::kEditClose);
    editorOpen = false;
}

int parameterCount() { return std::clamp(effect->numParams, 0, 65536); }

/// Answers one control request; false: it was Quit.
bool answer()
{
    bridge::Control& control = shared->control;
    if (lastBlob != nullptr) {
        CloseHandle(lastBlob);
        lastBlob = nullptr;
    }
    // What was asked, read once: the memory is not ours alone.
    const auto request = static_cast<bridge::Request>(control.request);
    const std::int32_t index = control.index;
    const std::int64_t value = control.value;
    const double number = control.number;
    const HANDLE hostBlob = handleOf(static_cast<std::uint32_t>(control.blob));
    control.result = 0;
    control.blob = 0;
    if (request == bridge::Request::Quit)
        return false;
    if (request == bridge::Request::Load) {
        if (effect == nullptr)
            control.result = load(control) ? 1 : 0;
        return true;
    }
    if (effect == nullptr)
        return true; // nothing loaded: every answer is "no"
    const bool known = index >= 0 && index < parameterCount();
    switch (request) {
    case bridge::Request::Activate:
        deactivate();
        control.result = activate(number, index) ? 1 : 0;
        break;
    case bridge::Request::Deactivate:
        deactivate();
        control.result = 1;
        break;
    case bridge::Request::ParameterName:
    case bridge::Request::ParameterLabel:
        control.text[0] = '\0';
        if (known) {
            pluginText(control.text,
                request == bridge::Request::ParameterName ? vst2::kGetParamName : vst2::kGetParamLabel,
                index);
            control.result = 1;
        }
        break;
    case bridge::Request::GetParameter:
        if (known) {
            control.number = static_cast<double>(effect->getParameter(effect, index));
            control.result = 1;
        }
        break;
    case bridge::Request::SetParameter:
        if (known && std::isfinite(number)) {
            effect->setParameter(effect, index, static_cast<float>(std::clamp(number, 0.0, 1.0)));
            control.result = 1;
        }
        break;
    case bridge::Request::GetParameters: {
        std::vector<float> values(static_cast<std::size_t>(parameterCount()));
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = effect->getParameter(effect, static_cast<std::int32_t>(i));
        control.result = values.empty()
            ? 1
            : (sendBlob(control, values.data(), static_cast<std::int64_t>(values.size() * sizeof(float)))
                      ? 1
                      : 0);
        break;
    }
    case bridge::Request::GetChunk: {
        if ((effect->flags & vst2::kProgramChunks) == 0)
            break;
        void* data = nullptr;
        const std::intptr_t size = dispatch(vst2::kGetChunk, 0, 0, &data);
        control.result = sendBlob(control, data, static_cast<std::int64_t>(size)) ? 1 : 0;
        break;
    }
    case bridge::Request::SetChunk: {
        if (hostBlob == nullptr)
            break;
        if ((effect->flags & vst2::kProgramChunks) != 0 && value > 0 && value <= bridge::kMaxBlobBytes) {
            const auto size = static_cast<SIZE_T>(value);
            void* view = MapViewOfFile(hostBlob, FILE_MAP_READ, 0, 0, size);
            MEMORY_BASIC_INFORMATION region{};
            if (view != nullptr && VirtualQuery(view, &region, sizeof(region)) != 0
                && region.RegionSize >= size && region.State == MEM_COMMIT) {
                // The plugin only reads it; the call takes a plain pointer.
                std::vector<char> copy(static_cast<const char*>(view), static_cast<const char*>(view) + size);
                dispatch(vst2::kSetChunk, 0, static_cast<std::intptr_t>(size), copy.data());
                control.result = 1;
            }
            if (view != nullptr)
                UnmapViewOfFile(view);
        }
        CloseHandle(hostBlob); // the host put it here for this request only
        break;
    }
    case bridge::Request::EditorOpen:
        closeEditor();
        if ((effect->flags & vst2::kHasEditor) == 0 || value == 0)
            break;
        editorOpen = true; // it may ask for a size while it opens
        // A window handle means the same in both processes; its upper half is not used.
        if (dispatch(vst2::kEditOpen, 0, 0, reinterpret_cast<void*>(static_cast<std::uintptr_t>(value))) == 0)
            editorOpen = false;
        control.result = editorOpen ? 1 : 0;
        break;
    case bridge::Request::EditorClose:
        closeEditor();
        control.result = 1;
        break;
    case bridge::Request::ProgramName:
        control.text[0] = '\0';
        if (index >= 0 && index < std::clamp(effect->numPrograms, 0, 4096)) {
            pluginText(control.text, vst2::kGetProgramNameIndexed, index);
            control.result = 1;
        }
        break;
    case bridge::Request::GetProgram:
        control.index
            = static_cast<std::int32_t>(std::clamp<std::intptr_t>(dispatch(vst2::kGetProgram), -1, 4096));
        control.result = 1;
        break;
    case bridge::Request::SetProgram:
        if (index >= 0 && index < std::clamp(effect->numPrograms, 0, 4096)) {
            dispatch(vst2::kSetProgram, 0, index);
            control.result = 1;
        }
        break;
    case bridge::Request::EditorSize: {
        vst2::Rect* rect = nullptr;
        if (!editorOpen || dispatch(vst2::kEditGetRect, 0, 0, &rect) == 0 || rect == nullptr)
            break;
        control.index = std::max(rect->right - rect->left, 0);
        control.value = std::max(rect->bottom - rect->top, 0);
        control.result = 1;
        break;
    }
    default:
        break;
    }
    return true;
}

/// One block: shared->audio in, shared->audio.out out.
void processBlock()
{
    bridge::Audio& audio = shared->audio;
    const int frames = std::clamp(audio.frames, 0, std::min(bridge::kMaxFrames, blockFrames));
    const auto count = static_cast<std::size_t>(frames);
    std::fill_n(audio.out[0], bridge::kMaxFrames, 0.0f);
    std::fill_n(audio.out[1], bridge::kMaxFrames, 0.0f);
    const int pairs = std::clamp(audio.extraPairs, 0, bridge::kMaxExtraPairs);
    for (int i = 0; i < 2 * pairs; ++i)
        std::fill_n(audio.extra[i], count, 0.0f);
    audio.madeNotes = 0;
    // A plugin that changed its channels since activation would write past
    // the buffers: silent until it is activated again.
    if (effect == nullptr || !active || frames == 0 || effect->numInputs != static_cast<int>(inputs.size())
        || effect->numOutputs != static_cast<int>(outputs.size()))
        return;

    const int changes = std::clamp(audio.parameterChanges, 0, bridge::kMaxParameterChanges);
    for (int i = 0; i < changes; ++i) {
        const bridge::ParameterChange change = audio.changes[i];
        if (change.index >= 0 && change.index < parameterCount() && std::isfinite(change.value))
            effect->setParameter(effect, change.index, std::clamp(change.value, 0.0f, 1.0f));
    }

    const bridge::SongTime song = audio.time;
    timeInfo = {};
    timeInfo.samplePos = song.samplePos;
    timeInfo.sampleRate = sampleRate;
    timeInfo.ppqPos = song.ppqPos;
    timeInfo.tempo = song.tempo;
    timeInfo.barStartPos = song.barStartPos;
    timeInfo.cycleStartPos = song.cycleStartPos;
    timeInfo.cycleEndPos = song.cycleEndPos;
    timeInfo.timeSigNumerator = song.timeSigNumerator;
    timeInfo.timeSigDenominator = song.timeSigDenominator;
    timeInfo.flags = song.flags;

    const int messages = std::clamp(audio.events, 0, bridge::kMaxEvents);
    for (int i = 0; i < messages; ++i) {
        vst2::MidiEvent& e = midi[static_cast<std::size_t>(i)];
        e.deltaFrames = std::clamp(audio.midi[i].frame, 0, frames - 1);
        std::memcpy(e.midiData, audio.midi[i].data, 4);
    }

    for (std::size_t ch = 0; ch < inputs.size(); ++ch) {
        float* dst = inputPointers[ch];
        if (inputs.size() == 1)
            for (std::size_t i = 0; i < count; ++i)
                dst[i] = 0.5f * (audio.in[0][i] + audio.in[1][i]);
        else if (ch < 2)
            std::copy_n(audio.in[ch], count, dst);
        else if (ch < 4 && audio.sidechain != 0)
            std::copy_n(audio.side[ch - 2], count, dst); // inputs 3 and 4: the sidechain
        else
            std::fill_n(dst, count, 0.0f);
    }
    for (float* dst : outputPointers)
        std::fill_n(dst, count, 0.0f);

    madeCount = 0;
    collecting = true;
    if (messages > 0) {
        events.numEvents = messages;
        dispatch(vst2::kProcessEvents, 0, 0, &events);
    }
    effect->processReplacing(effect, inputPointers.data(), outputPointers.data(), frames);
    collecting = false;
    if (!outputs.empty()) {
        std::copy_n(outputPointers[0], count, audio.out[0]);
        std::copy_n(outputPointers[outputs.size() > 1 ? 1 : 0], count, audio.out[1]);
    }
    // Its outputs after the first two, in pairs, as far as the host asked and it has them.
    for (int pair = 0; pair < pairs; ++pair) {
        const auto first = static_cast<std::size_t>(2 * pair + 2);
        if (first + 1 >= outputs.size())
            break; // no such pair: the silence written above stays
        std::copy_n(outputPointers[first], count, audio.extra[2 * pair]);
        std::copy_n(outputPointers[first + 1], count, audio.extra[2 * pair + 1]);
    }
    for (std::int32_t i = 0; i < madeCount; ++i)
        audio.made[i] = sentNotes[static_cast<std::size_t>(i)];
    audio.madeNotes = madeCount;
    shared->plugin.latency = effect->initialDelay;
}

struct AudioThread {
    HANDLE request = nullptr;
    HANDLE answered = nullptr;
    HANDLE stop = nullptr;
};

DWORD WINAPI audioMain(void* parameter)
{
    const auto* thread = static_cast<const AudioThread*>(parameter);
    audioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // The host's audio thread waits for this one: it must not be the
    // slower of the two. The system's audio class if it grants it, the
    // highest plain priority otherwise.
    DWORD task = 0;
    if (AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) == nullptr)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    // Numbers too small to matter count as zero: they are slow to compute
    // with, and plugin tails are full of them.
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush to zero, denormals are zero
    const HANDLE handles[2] = {thread->request, thread->stop};
    while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
        processBlock();
        SetEvent(thread->answered);
    }
    return 0;
}

struct WatchThread {
    HANDLE request = nullptr;
    HANDLE host = nullptr;
    HWND window = nullptr;
};

DWORD WINAPI watchMain(void* parameter)
{
    const auto* thread = static_cast<const WatchThread*>(parameter);
    const HANDLE handles[2] = {thread->request, thread->host};
    for (;;) {
        const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (woke == WAIT_OBJECT_0)
            PostMessageW(thread->window, kRequestMessage, 0, 0);
        else
            // The host is gone: nobody is left to answer or to save for,
            // and the main thread may be stuck inside the plugin.
            TerminateProcess(GetCurrentProcess(), 0);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kRequestMessage) {
        const bool more = answer();
        SetEvent(controlAnswer);
        if (!more) {
            quitAsked = true;
            PostQuitMessage(0);
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kIdleTimer) {
        if (editorOpen && effect != nullptr)
            dispatch(vst2::kEditIdle);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 7)
        return 2;
    // No crash or "missing DLL" dialogs: the host sees the process end.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDllDirectoryW(L""); // no DLL from the working directory
    const auto argument = [argv](int i) { return handleOf(_wcstoui64(argv[i], nullptr, 10)); };
    shared = static_cast<bridge::Shared*>(
        MapViewOfFile(argument(1), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Shared)));
    controlAnswer = argument(3);
    AudioThread audio{argument(4), argument(5), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (shared == nullptr || shared->magic != bridge::kMagic || shared->version != bridge::kVersion
        || audio.stop == nullptr)
        return 3;

    for (std::size_t i = 0; i < midi.size(); ++i) {
        midi[i].type = vst2::kMidiEventType;
        midi[i].byteSize = static_cast<std::int32_t>(sizeof(vst2::MidiEvent));
        events.pointers[i] = &midi[i];
    }
    OleInitialize(nullptr); // editors use it (drag and drop, the clipboard)

    // The hidden window requests come to, and the editor's idle time.
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"VloomPluginBridge";
    RegisterClassW(&windowClass);
    WatchThread watch{argument(2), argument(6),
        CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
            windowClass.hInstance, nullptr)};
    if (watch.window == nullptr)
        return 3;
    SetTimer(watch.window, kIdleTimer, 30, nullptr);
    HANDLE audioThread = CreateThread(nullptr, 0, audioMain, &audio, 0, nullptr);
    HANDLE watchThread = CreateThread(nullptr, 0, watchMain, &watch, 0, nullptr);
    if (audioThread == nullptr || watchThread == nullptr)
        return 3;

    // Until the host says quit. A plugin may post a quit message of its
    // own (some do as their editor window goes): that one is not for us.
    for (;;) {
        MSG message;
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got < 0 || (got == 0 && quitAsked))
            break;
        if (got == 0)
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    SetEvent(audio.stop);
    WaitForSingleObject(audioThread, 2000);
    if (effect != nullptr) {
        closeEditor();
        deactivate();
        dispatch(vst2::kClose);
    }
    // Ended, not returned from: the watch thread still waits, and unloading
    // the module would run plugin code that has nothing left to do but go
    // wrong.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
