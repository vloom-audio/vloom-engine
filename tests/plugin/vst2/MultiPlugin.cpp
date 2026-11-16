// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// "Test Multi", a VST 2 plugin for the host tests of what goes beyond a
// plugin's main sound (plugin/Vst2HostTests.cpp):
//
// - four inputs: the sound on 1 and 2, a sidechain on 3 and 4. The main
//   output is the sound * (1 - |sidechain|);
// - six outputs: after the main pair, a pair that carries the last pitch
//   bend (left, 0 .. 1) and the last value of controller 1 (right, 0 .. 1),
//   and a pair that carries 0.3;
// - for every note-on it gets, it sends the host a note-on an octave up, if
//   the host says it takes MIDI from plugins (as careful plugins ask first);
// - three programs, "Soft", "Hard" and "Wide".
#include "plugin/Vst2Abi.hpp"

#include <cmath>
#include <cstring>

#if defined(_WIN32)
#define VLOOM_TEST_EXPORT __declspec(dllexport)
#else
#define VLOOM_TEST_EXPORT __attribute__((visibility("default")))
#endif

namespace {

using namespace vloom::vst2;

struct Plugin {
    Effect effect{};
    HostCallback host = nullptr;
    float bend = 0.0f;
    float wheel = 0.0f;
    int program = 0;
    MidiEvent echo[16]{};
    int echoes = 0;
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
    static const char* const kPrograms[] = {"Soft", "Hard", "Wide"};
    Plugin* plugin = self(effect);
    switch (opcode) {
    case kClose:
        delete plugin;
        return 0;
    case kGetEffectName:
        copyText(ptr, "Test Multi");
        return 1;
    case kGetVendorString:
        copyText(ptr, "Vloom Tests");
        return 1;
    case kCanDo:
        return ptr != nullptr && std::strcmp(static_cast<const char*>(ptr), "sendVstMidiEvent") == 0 ? 1 : 0;
    case kGetProgram:
        return plugin->program;
    case kSetProgram:
        if (value >= 0 && value < 3)
            plugin->program = static_cast<int>(value);
        return 0;
    case kGetProgramNameIndexed:
        if (index < 0 || index >= 3)
            return 0;
        copyText(ptr, kPrograms[index]);
        return 1;
    case kProcessEvents: {
        const auto* events = static_cast<const Events*>(ptr);
        for (std::int32_t i = 0; events != nullptr && i < events->numEvents; ++i) {
            const MidiEvent* e = events->events[i];
            if (e->type != kMidiEventType)
                continue;
            const int status = static_cast<unsigned char>(e->midiData[0]) & 0xF0;
            const int one = static_cast<unsigned char>(e->midiData[1]) & 0x7F;
            const int two = static_cast<unsigned char>(e->midiData[2]) & 0x7F;
            if (status == 0xE0)
                plugin->bend = static_cast<float>(one | (two << 7)) / 16383.0f;
            else if (status == 0xB0 && one == 1)
                plugin->wheel = static_cast<float>(two) / 127.0f;
            else if (status == 0x90 && two > 0 && one + 12 < 128 && plugin->echoes < 16) {
                MidiEvent& out = plugin->echo[plugin->echoes++];
                out = *e;
                out.midiData[1] = static_cast<char>(one + 12);
            }
        }
        return 1;
    }
    default:
        return 0;
    }
}

void VLOOM_VST2_CALL setParameter(Effect*, std::int32_t, float) { }
float VLOOM_VST2_CALL getParameter(Effect*, std::int32_t) { return 0.0f; }

void VLOOM_VST2_CALL process(Effect* effect, float** inputs, float** outputs, std::int32_t frames)
{
    Plugin* plugin = self(effect);
    // What it made of this block's notes goes back to the host.
    char receives[] = "receiveVstMidiEvent";
    if (plugin->host(effect, kHostCanDo, 0, 0, receives, 0.0f) != 1)
        plugin->echoes = 0;
    if (plugin->echoes > 0) {
        struct {
            std::int32_t numEvents;
            std::intptr_t reserved;
            MidiEvent* events[16];
        } block{};
        block.numEvents = plugin->echoes;
        for (int i = 0; i < plugin->echoes; ++i)
            block.events[i] = &plugin->echo[i];
        plugin->host(effect, kHostProcessEvents, 0, 0, &block, 0.0f);
        plugin->echoes = 0;
    }
    for (std::int32_t i = 0; i < frames; ++i) {
        outputs[0][i] = inputs[0][i] * (1.0f - std::abs(inputs[2][i]));
        outputs[1][i] = inputs[1][i] * (1.0f - std::abs(inputs[3][i]));
        outputs[2][i] = plugin->bend;
        outputs[3][i] = plugin->wheel;
        outputs[4][i] = 0.3f;
        outputs[5][i] = 0.3f;
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
    effect.numPrograms = 3;
    effect.numParams = 0;
    effect.numInputs = 4;
    effect.numOutputs = 6;
    effect.flags = kCanReplacing;
    effect.object = plugin;
    effect.uniqueId = 0x564C5433; // 'VLT3'
    effect.version = 1000;
    return &effect;
}
