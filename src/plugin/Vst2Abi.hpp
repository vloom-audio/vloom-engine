// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// The binary interface of VST 2 plugins, as far as the host uses it: the
// plugin's structure, the calls both sides make, and what a block's events
// and song position look like. Written out here from what plugins and hosts
// do in practice; no Steinberg header is used or needed (the VST 2 SDK is no
// longer licensed). "VST" is a trademark of Steinberg Media Technologies GmbH.

#include <cstdint>

#if defined(_WIN32)
#define VLOOM_VST2_CALL __cdecl
#else
#define VLOOM_VST2_CALL
#endif

namespace vloom::vst2 {

struct Effect;

/// What the plugin calls the host with ("audio master").
using HostCallback = std::intptr_t(VLOOM_VST2_CALL*)(
    Effect* effect, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt);
using Dispatcher = std::intptr_t(VLOOM_VST2_CALL*)(
    Effect* effect, std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt);
using ProcessFn
    = void(VLOOM_VST2_CALL*)(Effect* effect, float** inputs, float** outputs, std::int32_t frames);
using ProcessDoubleFn
    = void(VLOOM_VST2_CALL*)(Effect* effect, double** inputs, double** outputs, std::int32_t frames);
using SetParameterFn = void(VLOOM_VST2_CALL*)(Effect* effect, std::int32_t index, float value);
using GetParameterFn = float(VLOOM_VST2_CALL*)(Effect* effect, std::int32_t index);

/// The module's entry point: "VSTPluginMain", or "main" in older plugins.
using EntryPoint = Effect*(VLOOM_VST2_CALL*)(HostCallback host);

constexpr std::int32_t kMagic = 0x56737450; ///< 'VstP'

/// A plugin instance, as the plugin hands it to the host.
struct Effect {
    std::int32_t magic;
    Dispatcher dispatcher;
    ProcessFn processAccumulating; ///< long out of use
    SetParameterFn setParameter;
    GetParameterFn getParameter;
    std::int32_t numPrograms;
    std::int32_t numParams;
    std::int32_t numInputs;
    std::int32_t numOutputs;
    std::int32_t flags;
    std::intptr_t hostData1; ///< for the host
    std::intptr_t hostData2; ///< for the host
    std::int32_t initialDelay; ///< latency in frames
    std::int32_t unused1;
    std::int32_t unused2;
    float unused3;
    void* object;
    void* user;
    std::int32_t uniqueId;
    std::int32_t version;
    ProcessFn processReplacing;
    ProcessDoubleFn processDoubleReplacing;
    char future[56];
};

/// Effect::flags.
enum Flags : std::int32_t {
    kHasEditor = 1 << 0,
    kCanReplacing = 1 << 4,
    kProgramChunks = 1 << 5, ///< state is an opaque chunk, not the parameters
    kIsSynth = 1 << 8,
};

/// Host to plugin (Effect::dispatcher).
enum Opcode : std::int32_t {
    kOpen = 0,
    kClose = 1,
    kGetParamLabel = 6,
    kGetParamName = 8,
    kSetSampleRate = 10,
    kSetBlockSize = 11,
    kMainsChanged = 12,
    kEditGetRect = 13,
    kEditOpen = 14,
    kEditClose = 15,
    kEditIdle = 19,
    kGetChunk = 23,
    kSetChunk = 24,
    kProcessEvents = 25,
    kGetPlugCategory = 35,
    kGetEffectName = 45,
    kGetVendorString = 47,
    kGetProductString = 48,
    kGetVendorVersion = 49,
    kStartProcess = 71,
    kStopProcess = 72,
};

constexpr std::intptr_t kCategoryShell = 10; ///< kGetPlugCategory: a file of many plugins

/// Plugin to host (HostCallback).
enum HostOpcode : std::int32_t {
    kHostAutomate = 0,
    kHostVersion = 1,
    kHostCurrentId = 2,
    kHostIdle = 3,
    kHostGetTime = 7,
    kHostIoChanged = 13,
    kHostSizeWindow = 15,
    kHostGetSampleRate = 16,
    kHostGetBlockSize = 17,
    kHostGetCurrentProcessLevel = 23,
    kHostGetVendorString = 32,
    kHostGetProductString = 33,
    kHostGetVendorVersion = 34,
    kHostCanDo = 37,
    kHostGetLanguage = 38,
    kHostUpdateDisplay = 42,
    kHostBeginEdit = 43,
    kHostEndEdit = 44,
};

/// kEditGetRect.
struct Rect {
    std::int16_t top;
    std::int16_t left;
    std::int16_t bottom;
    std::int16_t right;
};

/// kHostGetTime.
struct TimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos; ///< in quarter notes
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    std::int32_t timeSigNumerator;
    std::int32_t timeSigDenominator;
    std::int32_t smpteOffset;
    std::int32_t smpteFrameRate;
    std::int32_t samplesToNextClock;
    std::int32_t flags;
};

/// TimeInfo::flags.
enum TimeFlags : std::int32_t {
    kTransportPlaying = 1 << 1,
    kTransportCycleActive = 1 << 2,
    kPpqPosValid = 1 << 9,
    kTempoValid = 1 << 10,
    kBarsValid = 1 << 11,
    kCyclePosValid = 1 << 12,
    kTimeSigValid = 1 << 13,
};

constexpr std::int32_t kMidiEventType = 1;

/// One MIDI message of a block (kProcessEvents).
struct MidiEvent {
    std::int32_t type; ///< kMidiEventType
    std::int32_t byteSize; ///< sizeof(MidiEvent)
    std::int32_t deltaFrames; ///< its frame in the block
    std::int32_t flags;
    std::int32_t noteLength;
    std::int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

/// The header of a block's events; `events` goes on for numEvents pointers.
struct Events {
    std::int32_t numEvents;
    std::intptr_t reserved;
    MidiEvent* events[2];
};

} // namespace vloom::vst2
