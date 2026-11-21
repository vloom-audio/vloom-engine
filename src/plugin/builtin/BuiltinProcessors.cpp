// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/BuiltinProcessors.hpp"

#include "plugin/builtin/BuiltinEffect.hpp"

#include <vloom/dsp/NoteValue.hpp>
#include <vloom/dsp/Wavetable.hpp>
#include <vloom/dsp/effects/Analyzer.hpp>
#include <vloom/dsp/effects/Bitcrusher.hpp>
#include <vloom/dsp/effects/Chorus.hpp>
#include <vloom/dsp/effects/Compressor.hpp>
#include <vloom/dsp/effects/DeEsser.hpp>
#include <vloom/dsp/effects/Delay.hpp>
#include <vloom/dsp/effects/Eq.hpp>
#include <vloom/dsp/effects/Filter.hpp>
#include <vloom/dsp/effects/Flanger.hpp>
#include <vloom/dsp/effects/Gain.hpp>
#include <vloom/dsp/effects/Gate.hpp>
#include <vloom/dsp/effects/Limiter.hpp>
#include <vloom/dsp/effects/Phaser.hpp>
#include <vloom/dsp/effects/Reverb.hpp>
#include <vloom/dsp/effects/Saturator.hpp>
#include <vloom/dsp/effects/TransientShaper.hpp>
#include <vloom/dsp/effects/Tremolo.hpp>
#include <vloom/dsp/effects/Tuner.hpp>
#include <vloom/dsp/effects/Utility.hpp>
#include <vloom/dsp/instruments/DrumSampler.hpp>
#include <vloom/dsp/instruments/Sampler.hpp>
#include <vloom/dsp/instruments/Synth.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vloom {

namespace {

using Labels = std::vector<std::string>;

/// The tempo-synced note lengths ("1/8 D"), as kNoteValues lists them.
Labels noteValues()
{
    Labels list;
    for (const dsp::NoteValue& note : dsp::kNoteValues)
        list.emplace_back(note.name);
    return list;
}

/// MIDI keys 0 .. 127 by name, key 60 being C3 (vloom-dsp counts so: the
/// drum sampler's first pad, key 36, is C1).
Labels keyNames()
{
    static constexpr std::array<const char*, 12> kNames{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    Labels list;
    for (int key = 0; key < 128; ++key)
        list.push_back(
            std::string(kNames[static_cast<std::size_t>(key % 12)]) + std::to_string(key / 12 - 2));
    return list;
}

// One function per built-in with choices: the labels of parameter `id`.

Labels compressorLabels(std::uint32_t id)
{
    return id == dsp::Compressor::kDetector ? Labels{"Peak", "RMS"} : Labels{};
}

Labels deEsserLabels(std::uint32_t id)
{
    return id == dsp::DeEsser::kMode ? Labels{"Wideband", "Split-band"} : Labels{};
}

Labels delayLabels(std::uint32_t id)
{
    if (id == dsp::Delay::kMode)
        return {"Stereo", "Ping-pong"};
    if (id == dsp::Delay::kNote) {
        Labels list;
        for (int i = 0; i < dsp::Delay::kNoteCount; ++i)
            list.emplace_back(dsp::Delay::noteName(i));
        return list;
    }
    return {};
}

Labels filterLabels(std::uint32_t id)
{
    if (id == dsp::Filter::kType)
        return {"Low-pass", "High-pass", "Band-pass", "Notch"};
    if (id == dsp::Filter::kSlope)
        return {"12 dB/oct", "24 dB/oct"};
    return {};
}

Labels saturatorLabels(std::uint32_t id)
{
    if (id == dsp::Saturator::kCurve)
        return {"Soft", "Tube", "Tape", "Hard"};
    if (id == dsp::Saturator::kOversampling)
        return {"2x", "4x"};
    return {};
}

Labels chorusLabels(std::uint32_t id) { return id == dsp::Chorus::kNote ? noteValues() : Labels{}; }
Labels flangerLabels(std::uint32_t id) { return id == dsp::Flanger::kNote ? noteValues() : Labels{}; }
Labels phaserLabels(std::uint32_t id) { return id == dsp::Phaser::kNote ? noteValues() : Labels{}; }

Labels tremoloLabels(std::uint32_t id)
{
    if (id == dsp::Tremolo::kNote)
        return noteValues();
    if (id == dsp::Tremolo::kMode)
        return {"Tremolo", "Auto-pan"};
    if (id == dsp::Tremolo::kShape)
        return {"Sine", "Triangle", "Square"};
    return {};
}

Labels tunerLabels(std::uint32_t id)
{
    return id == dsp::Tuner::kInput ? Labels{"Left + right", "Left", "Right"} : Labels{};
}

Labels analyzerLabels(std::uint32_t id)
{
    using A = dsp::Analyzer;
    switch (id) {
    case A::kSpeed:
        return {"Slow", "Medium", "Fast"};
    case A::kResolution: {
        Labels list;
        for (int resolution = 0; resolution < A::kResolutions; ++resolution)
            list.push_back(std::to_string(A::fftSize(resolution)));
        return list;
    }
    case A::kChannel:
        return {"Left + right", "Left", "Right"};
    default:
        return {};
    }
}

Labels samplerLabels(std::uint32_t id)
{
    using S = dsp::Sampler;
    switch (id) {
    case S::kRootKey:
    case S::kKeyLow:
    case S::kKeyHigh:
        return keyNames();
    case S::kLoopMode:
        return {"Off", "Forward", "Ping-pong"};
    case S::kFilterType:
        return {"Off", "Low-pass", "Band-pass", "High-pass"};
    default:
        return {};
    }
}

Labels drumLabels(std::uint32_t id)
{
    using D = dsp::DrumSampler;
    if (id < D::kPadBase)
        return {};
    const std::uint32_t field = (id - D::kPadBase) % D::kPadStride;
    switch (field) {
    case D::kKey:
        return keyNames();
    case D::kChoke: {
        Labels list{"Off"};
        for (int group = 1; group <= D::kChokeGroups; ++group)
            list.push_back(std::to_string(group));
        return list;
    }
    case D::kMode:
        return {"One-shot", "Gate"};
    default:
        return {};
    }
}

Labels synthLabels(std::uint32_t id)
{
    using S = dsp::Synth;
    switch (id) {
    case S::kATable:
    case S::kBTable: {
        Labels list;
        for (int shape = 0; shape < dsp::Wavetable::kShapes; ++shape)
            list.emplace_back(dsp::Wavetable::name(static_cast<dsp::Wavetable::Shape>(shape)));
        return list;
    }
    case S::kFilterType:
        return {"Low-pass", "Band-pass", "High-pass"};
    case S::kLfo1Shape:
    case S::kLfo2Shape:
    case S::kLfo3Shape:
    case S::kLfo4Shape:
        return {"Sine", "Triangle", "Saw", "Square", "Random"};
    case S::kEnv2Target:
    case S::kEnv3Target:
    case S::kLfo1Target:
    case S::kLfo2Target:
    case S::kLfo3Target:
    case S::kLfo4Target:
    case S::kMacro1Target:
    case S::kMacro2Target:
    case S::kMacro3Target:
    case S::kMacro4Target:
        return {"Off", "Cutoff", "A Position", "B Position", "Pitch", "Level"};
    default:
        return {};
    }
}

struct Entry {
    PluginInfo (*info)();
    std::unique_ptr<AudioProcessor> (*create)();
};

template <dsp::Effect Effect, const builtin::Extras& kExtras> constexpr Entry entry()
{
    return Entry{[] { return builtin::BuiltinEffect<Effect>::describe(kExtras); },
        []() -> std::unique_ptr<AudioProcessor> {
            return std::make_unique<builtin::BuiltinEffect<Effect>>(kExtras);
        }};
}

// What the lists show besides the name: a category, and labels for choices.
constexpr builtin::Extras kDynamics{"Dynamics"};
constexpr builtin::Extras kCompressor{"Dynamics", compressorLabels};
constexpr builtin::Extras kDeEsser{"Dynamics", deEsserLabels};
constexpr builtin::Extras kEqFilter{"EQ/Filter"};
constexpr builtin::Extras kFilter{"EQ/Filter", filterLabels};
constexpr builtin::Extras kDelay{"Delay/Reverb", delayLabels};
constexpr builtin::Extras kReverb{"Delay/Reverb"};
constexpr builtin::Extras kChorus{"Modulation", chorusLabels};
constexpr builtin::Extras kFlanger{"Modulation", flangerLabels};
constexpr builtin::Extras kPhaser{"Modulation", phaserLabels};
constexpr builtin::Extras kTremolo{"Modulation", tremoloLabels};
constexpr builtin::Extras kSaturator{"Distortion", saturatorLabels};
constexpr builtin::Extras kDistortion{"Distortion"};
constexpr builtin::Extras kUtility{"Utility"};
constexpr builtin::Extras kTuner{"Analysis", tunerLabels};
constexpr builtin::Extras kAnalyzer{"Analysis", analyzerLabels};
constexpr builtin::Extras kSynth{"Synth", synthLabels};
constexpr builtin::Extras kSampler{"Sampler", samplerLabels};
constexpr builtin::Extras kDrums{"Drums", drumLabels};

// One line per built-in. Ids are stable: saved projects refer to them.
constexpr std::array kBuiltins{
    // Effects
    entry<dsp::Compressor, kCompressor>(),
    entry<dsp::Limiter, kDynamics>(),
    entry<dsp::Gate, kDynamics>(),
    entry<dsp::DeEsser, kDeEsser>(),
    entry<dsp::TransientShaper, kDynamics>(),
    entry<dsp::Eq, kEqFilter>(),
    entry<dsp::Filter, kFilter>(),
    entry<dsp::Delay, kDelay>(),
    entry<dsp::Reverb, kReverb>(),
    entry<dsp::Chorus, kChorus>(),
    entry<dsp::Flanger, kFlanger>(),
    entry<dsp::Phaser, kPhaser>(),
    entry<dsp::Tremolo, kTremolo>(),
    entry<dsp::Saturator, kSaturator>(),
    entry<dsp::Bitcrusher, kDistortion>(),
    entry<dsp::Gain, kUtility>(),
    entry<dsp::Utility, kUtility>(),
    entry<dsp::Tuner, kTuner>(),
    entry<dsp::Analyzer, kAnalyzer>(),
    // Instruments
    entry<dsp::Synth, kSynth>(),
    entry<dsp::Sampler, kSampler>(),
    entry<dsp::DrumSampler, kDrums>(),
};

} // namespace

std::vector<PluginInfo> builtinProcessors()
{
    std::vector<PluginInfo> list;
    list.reserve(kBuiltins.size());
    for (const Entry& builtin : kBuiltins)
        list.push_back(builtin.info());
    return list;
}

std::unique_ptr<AudioProcessor> createBuiltinProcessor(std::string_view id)
{
    for (const Entry& builtin : kBuiltins)
        if (builtin.info().id == id)
            return builtin.create();
    return nullptr;
}

} // namespace vloom
