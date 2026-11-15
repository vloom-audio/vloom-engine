// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <vloom/dsp/Processor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vloom::builtin {

inline constexpr std::array<char, 4> kStateMagic{'V', 'L', 'B', '1'};

/// Wraps a vloom-dsp effect or instrument as an AudioProcessor (ADR 0008).
/// An instrument (dsp::Instrument: it has noteOn() and noteOff()) is listed
/// as one and is given the block's notes; an effect ignores them.
///
/// - Parameter changes and notes are applied at their frame: the block is
///   split there (a change before a note at the same frame, so the note
///   starts with it).
/// - The latest value of each parameter is kept in an atomic, so saveState()
///   on the control thread sees what the audio thread applied.
/// - loadState() hands the values to the audio thread for its next block.
template <dsp::Effect Effect> class BuiltinEffect final : public AudioProcessor {
public:
    BuiltinEffect()
        : values_(std::make_unique<std::atomic<double>[]>(specs().size()))
    {
        for (std::size_t i = 0; i < specs().size(); ++i)
            values_[i].store(specs()[i].defaultValue, std::memory_order_relaxed);
    }

    [[nodiscard]] PluginInfo info() const override
    {
        const dsp::ProcessorSpec spec = Effect::spec();
        return PluginInfo{spec.id, spec.name, "Vloom Audio", spec.version,
            kInstrument ? PluginInfo::Kind::Instrument : PluginInfo::Kind::Effect};
    }

    [[nodiscard]] std::vector<ParameterInfo> parameters() const override
    {
        std::vector<ParameterInfo> list;
        list.reserve(specs().size());
        for (const dsp::ParameterSpec& p : specs())
            list.push_back(ParameterInfo{
                p.id, p.name, p.unit, p.minValue, p.maxValue, p.defaultValue, p.stepped, p.group});
        return list;
    }

    bool activate(double sampleRate, int maxBlockFrames) override
    {
        if (!(sampleRate > 0.0) || !std::isfinite(sampleRate) || maxBlockFrames <= 0)
            return false;
        // Not processing now (ADR 0006): the effect is ours to set up.
        pending_.store(false, std::memory_order_relaxed);
        applyValues();
        effect_.prepare(sampleRate, maxBlockFrames); // starts at the values, no glide
        latency_ = std::max(effect_.latencyFrames(), 0);
        return true;
    }

    [[nodiscard]] int latencyFrames() const override { return latency_; }

    [[nodiscard]] std::vector<std::uint8_t> saveState() const override
    {
        const auto count = static_cast<std::uint32_t>(specs().size());
        std::vector<std::uint8_t> state;
        state.reserve(kStateMagic.size() + 4 + count * 12);
        state.insert(state.end(), kStateMagic.begin(), kStateMagic.end());
        putLe(state, count);
        for (std::size_t i = 0; i < specs().size(); ++i) {
            putLe(state, specs()[i].id);
            std::uint64_t bits = 0;
            const double value = values_[i].load(std::memory_order_relaxed);
            std::memcpy(&bits, &value, sizeof bits);
            putLe(state, bits);
        }
        return state;
    }

    bool loadState(std::span<const std::uint8_t> state) override
    {
        if (state.size() < kStateMagic.size() + 4
            || !std::equal(kStateMagic.begin(), kStateMagic.end(), state.begin()))
            return false;
        const auto count = getLe<std::uint32_t>(state.subspan(kStateMagic.size()));
        const std::span<const std::uint8_t> entries = state.subspan(kStateMagic.size() + 4);
        if (entries.size() != static_cast<std::size_t>(count) * 12)
            return false;
        for (std::uint32_t e = 0; e < count; ++e) {
            const auto entry = entries.subspan(static_cast<std::size_t>(e) * 12, 12);
            const auto id = getLe<std::uint32_t>(entry);
            const auto bits = getLe<std::uint64_t>(entry.subspan(4));
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof value);
            const int index = indexOf(id);
            if (index >= 0 && std::isfinite(value)) // unknown: from a newer version
                values_[static_cast<std::size_t>(index)].store(
                    clampTo(specs()[static_cast<std::size_t>(index)], value), std::memory_order_relaxed);
        }
        pending_.store(true, std::memory_order_release); // the audio thread applies them
        return true;
    }

    void process(const ProcessContext& context) noexcept override
    {
        if (pending_.exchange(false, std::memory_order_acquire))
            applyValues();
        const int numChannels = std::clamp(context.numChannels, 0, kMaxChannels);
        const int numFrames = std::max(context.numFrames, 0);
        const std::span<const ParameterEvent> changes = context.parameters;
        const std::span<const NoteEvent> notes = kInstrument ? context.notes : std::span<const NoteEvent>();
        int position = 0;
        std::size_t change = 0;
        std::size_t note = 0;
        // Both lists are in frame order: take whichever comes first.
        while (change < changes.size() || note < notes.size()) {
            constexpr int kNever = std::numeric_limits<int>::max();
            const int changeFrame
                = change < changes.size() ? std::clamp(changes[change].frame, position, numFrames) : kNever;
            const int noteFrame
                = note < notes.size() ? std::clamp(notes[note].frame, position, numFrames) : kNever;
            const int frame = std::min(changeFrame, noteFrame);
            run(context.channels, numChannels, position, frame - position);
            position = frame;
            if (changeFrame <= noteFrame) {
                const ParameterEvent& event = changes[change++];
                const int index = indexOf(event.id);
                if (index < 0 || !std::isfinite(event.value))
                    continue;
                const double value = clampTo(specs()[static_cast<std::size_t>(index)], event.value);
                values_[static_cast<std::size_t>(index)].store(value, std::memory_order_relaxed);
                effect_.setParameter(event.id, value);
            } else {
                play(notes[note++]);
            }
        }
        run(context.channels, numChannels, position, numFrames - position);
    }

    void reset() noexcept override { effect_.reset(); }

    [[nodiscard]] std::optional<double> parameterValue(std::uint32_t id) const override
    {
        const int index = indexOf(id);
        if (index < 0)
            return std::nullopt;
        return values_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
    }

    /// Takes the value at once, so saveState() and parameterValue() have it
    /// while the audio thread is stopped; the audio thread applies it with
    /// its next block. True: the host does not queue it as an event, where
    /// an older change could arrive after a newer one taken here.
    bool parameterChangedByHost(std::uint32_t id, double value) override
    {
        const int index = indexOf(id);
        if (index >= 0 && std::isfinite(value)) {
            values_[static_cast<std::size_t>(index)].store(
                clampTo(specs()[static_cast<std::size_t>(index)], value), std::memory_order_relaxed);
            pending_.store(true, std::memory_order_release);
        }
        return true; // unknown ids and NaN are dropped here too
    }

    /// The value of parameter `id` (any thread); its default if unknown.
    [[nodiscard]] double value(std::uint32_t id) const noexcept
    {
        const int index = indexOf(id);
        return index < 0 ? 0.0 : values_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
    }

private:
    static constexpr int kMaxChannels = 8;
    static constexpr bool kInstrument = dsp::Instrument<Effect>;

    /// Starts or ends a note of an instrument.
    void play(const NoteEvent& note) noexcept
    {
        if constexpr (dsp::Instrument<Effect>) {
            if (note.type == NoteEvent::Type::On)
                effect_.noteOn(note.key, note.velocity);
            else
                effect_.noteOff(note.key);
        } else {
            (void)note;
        }
    }

    [[nodiscard]] static std::span<const dsp::ParameterSpec> specs() noexcept { return Effect::parameters(); }

    [[nodiscard]] static int indexOf(std::uint32_t id) noexcept
    {
        const auto list = specs();
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].id == id)
                return static_cast<int>(i);
        return -1;
    }

    [[nodiscard]] static double clampTo(const dsp::ParameterSpec& spec, double value) noexcept
    {
        value = std::clamp(value, spec.minValue, spec.maxValue);
        return spec.stepped ? std::round(value) : value;
    }

    void applyValues() noexcept
    {
        for (std::size_t i = 0; i < specs().size(); ++i)
            effect_.setParameter(specs()[i].id, values_[i].load(std::memory_order_relaxed));
    }

    void run(float* const* channels, int numChannels, int offset, int frames) noexcept
    {
        if (frames <= 0 || channels == nullptr)
            return;
        std::array<float*, kMaxChannels> part{};
        for (int ch = 0; ch < numChannels; ++ch)
            part[static_cast<std::size_t>(ch)] = channels[ch] + offset;
        effect_.process(part.data(), numChannels, frames);
    }

    template <typename T> static void putLe(std::vector<std::uint8_t>& out, T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
            out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    template <typename T> [[nodiscard]] static T getLe(std::span<const std::uint8_t> in) noexcept
    {
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<T>(static_cast<T>(in[i]) << (8 * i));
        return value;
    }

    Effect effect_; ///< audio thread, once active
    std::unique_ptr<std::atomic<double>[]> values_; ///< per parameter, in specs() order
    std::atomic<bool> pending_{false}; ///< values set off the audio thread wait for it
    int latency_ = 0; ///< read at activation
};

} // namespace vloom::builtin
