// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// A real VST 2 plugin for the host tests (plugin/Vst2HostTests.cpp). A VST 2
// file holds one plugin, so this source is built twice:
//
// - "Test Level" (effect, the default): output = input * 2 * parameter 0
//   (default 0.5 = unity), stereo in and out, 7 frames of latency, state as
//   parameter values. Its editor (no real window: it only records the
//   parent) is 300 x 200; as it opens it asks the host for 400 x 300 and
//   edits parameter 0 to 0.25.
// - "Test Keys" (instrument, with VLOOM_TEST_VST2_KEYS): no input; left =
//   0.1 per key held, counted from the block's MIDI events; right = the time
//   signature's beats per bar / 100, as the host tells it. Silent if the
//   host does not say the block comes from the audio thread. State as a
//   chunk of 4 bytes. Key 127 ends the process it runs in at once, for
//   the test of a bridge that dies; key 126 makes that one block take 2.6
//   seconds.
#include "plugin/Vst2Abi.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define VLOOM_TEST_EXPORT __declspec(dllexport)
#else
#define VLOOM_TEST_EXPORT __attribute__((visibility("default")))
#endif

namespace {

using namespace vloom::vst2;

#if defined(VLOOM_TEST_VST2_KEYS)
constexpr bool kKeys = true;
#else
constexpr bool kKeys = false;
#endif

struct Plugin {
    Effect effect{};
    HostCallback host = nullptr;
    float level = 0.5f;
    bool held[128]{};
    int count = 0;
    char chunk[4] = {'t', 'u', 'n', 'e'};
    void* parent = nullptr;
    Rect rect{0, 0, 200, 300};
};

Plugin* self(Effect* effect) { return static_cast<Plugin*>(effect->object); }

void copyText(void* ptr, const char* text)
{
    if (ptr != nullptr)
        std::strcpy(static_cast<char*>(ptr), text);
}

std::intptr_t VLOOM_VST2_CALL dispatcher(
    Effect* effect, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float)
{
    Plugin* plugin = self(effect);
    switch (opcode) {
    case kClose:
        delete plugin;
        return 0;
    case kGetParamName:
        copyText(ptr, index == 0 && !kKeys ? "Level" : "");
        return 0;
    case kGetParamLabel:
        copyText(ptr, index == 0 && !kKeys ? "x" : "");
        return 0;
    case kGetEffectName:
        copyText(ptr, kKeys ? "Test Keys" : "Test Level");
        return 1;
    case kGetVendorString:
        copyText(ptr, "Vloom Tests");
        return 1;
    case kEditGetRect:
        if (kKeys || ptr == nullptr)
            return 0;
        *static_cast<Rect**>(ptr) = &plugin->rect;
        return 1;
    case kEditOpen:
        if (kKeys || ptr == nullptr)
            return 0;
        plugin->parent = ptr;
        if (plugin->host(effect, kHostSizeWindow, 400, 300, nullptr, 0.0f) != 0)
            plugin->rect = {0, 0, 300, 400};
        plugin->level = 0.25f; // as if the user turned the editor's knob
        plugin->host(effect, kHostAutomate, 0, 0, nullptr, 0.25f);
        return 1;
    case kEditClose:
        plugin->parent = nullptr;
        return 0;
    case kGetChunk:
        if (!kKeys || ptr == nullptr)
            return 0;
        *static_cast<void**>(ptr) = plugin->chunk;
        return sizeof(plugin->chunk);
    case kSetChunk:
        if (kKeys && ptr != nullptr && value == static_cast<std::intptr_t>(sizeof(plugin->chunk)))
            std::memcpy(plugin->chunk, ptr, sizeof(plugin->chunk));
        return 0;
    case kProcessEvents: {
        const auto* events = static_cast<const Events*>(ptr);
        for (std::int32_t i = 0; events != nullptr && i < events->numEvents; ++i) {
            const MidiEvent* e = events->events[i];
            if (e->type != kMidiEventType)
                continue;
            const int status = static_cast<unsigned char>(e->midiData[0]) & 0xF0;
            const int key = static_cast<unsigned char>(e->midiData[1]) & 0x7F;
            if (status != 0x90 && status != 0x80)
                continue;
            const bool on = status == 0x90 && e->midiData[2] != 0;
            if (on && key == 126) // the test of a plugin that is slow for once, not gone
                std::this_thread::sleep_for(std::chrono::milliseconds(2600));
            if (on && key == 127)
                std::_Exit(3); // the test of a plugin that takes its process down
            plugin->count += (on ? 1 : 0) - (plugin->held[key] ? 1 : 0);
            plugin->held[key] = on;
        }
        return 1;
    }
    default:
        return 0;
    }
}

void VLOOM_VST2_CALL setParameter(Effect* effect, std::int32_t index, float value)
{
    if (index == 0)
        self(effect)->level = value;
}

float VLOOM_VST2_CALL getParameter(Effect* effect, std::int32_t index)
{
    return index == 0 ? self(effect)->level : 0.0f;
}

void VLOOM_VST2_CALL process(Effect* effect, float** inputs, float** outputs, std::int32_t frames)
{
    Plugin* plugin = self(effect);
    if (!kKeys) {
        for (std::int32_t i = 0; i < frames; ++i) {
            outputs[0][i] = inputs[0][i] * 2.0f * plugin->level;
            outputs[1][i] = inputs[1][i] * 2.0f * plugin->level;
        }
        return;
    }
    // Events arrive before the block: every note counts from its first frame.
    const bool audioThread = plugin->host(effect, kHostGetCurrentProcessLevel, 0, 0, nullptr, 0.0f) == 2;
    const auto* time
        = reinterpret_cast<const TimeInfo*>(plugin->host(effect, kHostGetTime, 0, 0, nullptr, 0.0f));
    const float beats = time != nullptr && (time->flags & kTimeSigValid) != 0
        ? static_cast<float>(time->timeSigNumerator) / 100.0f
        : 0.0f;
    for (std::int32_t i = 0; i < frames; ++i) {
        outputs[0][i] = audioThread ? 0.1f * static_cast<float>(plugin->count) : 0.0f;
        outputs[1][i] = beats;
    }
}

} // namespace

extern "C" VLOOM_TEST_EXPORT vloom::vst2::Effect* VSTPluginMain(vloom::vst2::HostCallback host)
{
    if (host == nullptr || host(nullptr, kHostVersion, 0, 0, nullptr, 0.0f) < 2000)
        return nullptr;
    auto* plugin = new Plugin;
    plugin->host = host;
    Effect& effect = plugin->effect;
    effect.magic = kMagic;
    effect.dispatcher = dispatcher;
    effect.setParameter = setParameter;
    effect.getParameter = getParameter;
    effect.processReplacing = process;
    effect.numPrograms = 1;
    effect.numParams = kKeys ? 0 : 1;
    effect.numInputs = kKeys ? 0 : 2;
    effect.numOutputs = 2;
    effect.flags = kKeys ? kCanReplacing | kIsSynth | kProgramChunks : kCanReplacing | kHasEditor;
    effect.initialDelay = kKeys ? 0 : 7;
    effect.object = plugin;
    effect.uniqueId = kKeys ? 0x564C5432 : 0x564C5431; // 'VLT2', 'VLT1'
    effect.version = 1000;
    return &effect;
}
