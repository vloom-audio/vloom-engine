// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// What the engine and the plugin bridge (tools/vloom-plugin-bridge) share
// (ADR 0011): one block of memory, laid out the same in a 32-bit and a
// 64-bit process, and four events that say "a request is there" and "it is
// answered", for the control thread and for the audio thread.
//
// None of these objects has a name. The host makes them, lets the bridge
// inherit exactly these handles, and gives it their values on its command
// line: <memory> <control request> <control answer> <audio request>
// <audio answer> <host process>, as decimal numbers.
//
// Only fixed-width numbers and arrays: no pointers, no size_t. Each side
// treats what the other wrote as untrusted and clamps it before use.

#include <cstdint>

namespace vloom::bridge {

inline constexpr std::uint32_t kMagic = 0x564C4252; ///< 'VLBR'
inline constexpr std::uint32_t kVersion = 3;

inline constexpr std::int32_t kMaxFrames = 8192; ///< the longest block
inline constexpr std::int32_t kMaxEvents = 512;
inline constexpr std::int32_t kMaxParameterChanges = 256;
inline constexpr std::int32_t kMaxNotices = 64; ///< a power of two
inline constexpr std::int32_t kMaxExtraPairs = 8; ///< stereo outputs after the main one that cross
inline constexpr std::int32_t kMaxMadeNotes = 256; ///< notes the plugin sends back in one block
inline constexpr std::int32_t kTextSize = 512;
inline constexpr std::int64_t kMaxBlobBytes = std::int64_t{1} << 30; ///< 1 GiB

/// What the control thread asks of the bridge.
enum class Request : std::int32_t {
    None = 0,
    Load, ///< text: the plugin file (UTF-8). Fills Shared::plugin; result 0 with text = why not
    Activate, ///< number: the sample rate, index: frames per block
    Deactivate,
    ParameterName, ///< index -> text
    ParameterLabel, ///< index -> text
    GetParameter, ///< index -> number
    SetParameter, ///< index, number
    GetParameters, ///< -> a blob of floats, one per parameter
    GetChunk, ///< -> a blob (value: its size)
    SetChunk, ///< blob, value: the size of the blob the host made
    EditorOpen, ///< value: the parent window
    EditorClose,
    EditorSize, ///< -> index: width, value: height; result 0: none
    ProgramName, ///< index -> text
    GetProgram, ///< -> index
    SetProgram, ///< index
    Quit,
};

/// One request and its answer.
struct Control {
    std::int32_t request;
    std::int32_t index;
    std::int64_t value;
    double number;
    std::int64_t result;
    /// A blob is a second block of memory for data too large for this one.
    /// This is its handle in the bridge process: the bridge's own for an
    /// answer (the host copies it from there), one the host put into the
    /// bridge for a request (the bridge closes it). 0: none.
    std::int32_t blob;
    std::int32_t padding;
    char text[kTextSize];
};

/// What the plugin is, filled by Request::Load.
struct PluginFacts {
    std::int32_t parameters;
    std::int32_t inputs;
    std::int32_t outputs;
    std::int32_t flags; ///< the plugin's own (vst2::Flags)
    std::int32_t uniqueId;
    std::int32_t version;
    std::int32_t latency; ///< rewritten after every block
    std::int32_t programs; ///< how many it lists
    std::int32_t makesNotes; ///< 1: it says it sends MIDI to the host
    std::int32_t padding;
    char name[kTextSize];
    char vendor[kTextSize];
};

struct MidiMessage {
    std::int32_t frame;
    std::uint8_t data[4];
};

struct ParameterChange {
    std::int32_t index;
    float value;
};

/// The song as of a block (vst2::TimeInfo without its unused fields).
struct SongTime {
    double samplePos;
    double sampleRate;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    std::int32_t timeSigNumerator;
    std::int32_t timeSigDenominator;
    std::int32_t flags;
    std::int32_t padding;
};

/// One block: the host fills it and signals; the bridge writes `out`.
struct Audio {
    std::int32_t frames;
    std::int32_t events;
    std::int32_t parameterChanges;
    std::int32_t sidechain; ///< 1: `side` carries one
    std::int32_t extraPairs; ///< in: how many of `extra` the host wants filled
    std::int32_t madeNotes; ///< out: how many of `made` the plugin sent
    std::int32_t padding[2];
    SongTime time;
    MidiMessage midi[kMaxEvents];
    ParameterChange changes[kMaxParameterChanges];
    float in[2][kMaxFrames];
    float out[2][kMaxFrames];
    float side[2][kMaxFrames]; ///< the sidechain, for the plugin's inputs 3 and 4
    float extra[2 * kMaxExtraPairs][kMaxFrames]; ///< its outputs 3 and 4, 5 and 6, ...
    MidiMessage made[kMaxMadeNotes]; ///< the notes it sent the host during the block
};

/// What the plugin tells the host on its own: an edit in its editor, or a
/// size it wants for the editor.
struct Notice {
    std::int32_t kind; ///< 1: edit (index, value), 2: size (index x size)
    std::int32_t index;
    float value;
    std::int32_t size;
};

/// A ring the bridge writes and the host reads. The counters only grow; a
/// slot is index % kMaxNotices. Written with interlocked operations.
struct Notices {
    volatile std::int32_t written;
    volatile std::int32_t read;
    Notice items[kMaxNotices];
};

struct Shared {
    std::uint32_t magic;
    std::uint32_t version;
    Control control;
    PluginFacts plugin;
    Notices notices;
    Audio audio;
};

// Both builds of the bridge must agree with the host to the byte.
static_assert(sizeof(Control) == 552);
static_assert(sizeof(PluginFacts) == 1064);
static_assert(sizeof(SongTime) == 72);
static_assert(sizeof(Notices) == 1032);
static_assert(sizeof(Audio) == 729192);
static_assert(sizeof(Shared) == 731848);

} // namespace vloom::bridge
