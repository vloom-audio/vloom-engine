// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "plugin/builtin/BuiltinSamples.hpp"
#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <vloom/dsp/Processor.hpp>
#include <vloom/dsp/Sample.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vloom::builtin {

inline constexpr std::array<char, 4> kStateMagic{'V', 'L', 'B', '1'};

/// A processor that reports how much it turns the sound down (vloom-dsp's
/// meter convention: dB, >= 0, any thread).
template <typename T>
concept GainReductionMeter = requires(const T processor) {
    { processor.gainReductionDb() } noexcept -> std::convertible_to<float>;
};

/// A processor that reports whether it is open (vloom-dsp's Gate; any
/// thread).
template <typename T>
concept OpenStateMeter = requires(const T processor) {
    { processor.isOpen() } noexcept -> std::convertible_to<bool>;
};

/// A value a built-in shows (ADR 0019): how a host lists it, and how to
/// read it from the effect on any thread.
template <typename Effect> struct Readout {
    const char* id = "";
    const char* name = "";
    const char* unit = "";
    bool toggle = false;
    float (*value)(const Effect& effect) noexcept = nullptr;
};

/// The table's lines for meters vloom-dsp also publishes as readouts: an
/// effect with dsp::HasReadouts reports them there instead, so no id is
/// listed twice (ADR 0019).
template <typename Effect>
inline constexpr bool kTableGainReduction = GainReductionMeter<Effect> && !dsp::HasReadouts<Effect>;
template <typename Effect>
inline constexpr bool kTableOpenState = OpenStateMeter<Effect> && !dsp::HasReadouts<Effect>;

template <typename Effect>
inline constexpr std::size_t kReadoutCount
    = (kTableGainReduction<Effect> ? 1U : 0U) + (kTableOpenState<Effect> ? 1U : 0U);

/// The one place where vloom-dsp capabilities become readouts (ADR 0019).
/// Every one must be readable from any thread without locking.
///
/// vloom-dsp's HasReadouts (readoutSpecs() and readout(i)) plugs in here:
/// the adapter lists its readouts after these (dspReadoutInfo()), and an
/// effect that reports its gain reduction or open state through it drops
/// out of the matching line below.
template <typename Effect>
[[nodiscard]] constexpr std::array<Readout<Effect>, kReadoutCount<Effect>> readoutTable() noexcept
{
    std::array<Readout<Effect>, kReadoutCount<Effect>> table{};
    std::size_t next = 0;
    if constexpr (kTableGainReduction<Effect>)
        table[next++] = Readout<Effect>{"gainReduction", "Gain Reduction", "dB", false,
            [](const Effect& effect) noexcept { return static_cast<float>(effect.gainReductionDb()); }};
    if constexpr (kTableOpenState<Effect>)
        table[next++] = Readout<Effect>{"gateOpen", "Gate Open", "", true,
            [](const Effect& effect) noexcept { return effect.isOpen() ? 1.0f : 0.0f; }};
    (void)next;
    return table;
}

/// How a vloom-dsp readout (HasReadouts) is listed: under the dsp's own id
/// and name, except the gain reduction and a gate's open state, which keep
/// the ids the table gave them before ("gainReduction", "gateOpen"), so a UI
/// does not notice the move. A stepped 0 .. 1 readout is a toggle.
[[nodiscard]] inline ReadoutInfo dspReadoutInfo(const dsp::ReadoutSpec& spec)
{
    const std::string_view id = spec.id;
    const bool toggle = spec.stepped && spec.minValue == 0.0 && spec.maxValue == 1.0;
    if (id == "gain_reduction_db")
        return ReadoutInfo{"gainReduction", "Gain Reduction", spec.unit, toggle};
    if (id == "open")
        return ReadoutInfo{"gateOpen", "Gate Open", spec.unit, toggle};
    return ReadoutInfo{spec.id, spec.name, spec.unit, toggle};
}

/// What the registry knows about a built-in besides its vloom-dsp spec.
struct Extras {
    const char* category = ""; ///< PluginInfo::category
    /// The names of a stepped choice parameter's values (ParameterInfo::
    /// valueLabels); empty for others. Null: none at all.
    std::vector<std::string> (*labels)(std::uint32_t id) = nullptr;
};

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
///
/// Optional vloom-dsp capabilities, found at compile time:
/// - dsp::SidechainEffect: hasSidechain(); processWithSidechain() while the
///   host sends a key (ProcessContext::sidechain), process() otherwise.
/// - dsp::TempoAware: setTempo() with the transport's tempo before the first
///   block after activate() and whenever it changes.
/// - Latency that changes with a parameter (limiter, saturator): read again
///   on the audio thread after every change, for latencyFrames() and so
///   PluginInstance::updateLatency().
/// - dsp::SampleSlots: the sample slot API of AudioProcessor, with the
///   paths in the state (BuiltinSamples.hpp); idle() releases replaced
///   samples.
/// - gainReductionDb() for a meter.
/// - Readouts (ADR 0019): the capabilities above that are values to show
///   (gain reduction, a gate's open state), mapped in one place,
///   readoutTable(), then dsp::HasReadouts' readoutSpecs() (the tuner, the
///   analyzer, the dynamics), read lock-free through readout().
template <dsp::Effect Effect> class BuiltinEffect final : public AudioProcessor {
public:
    explicit BuiltinEffect(Extras extras = {})
        : extras_(extras)
        , values_(std::make_unique<std::atomic<double>[]>(specs().size()))
    {
        for (std::size_t i = 0; i < specs().size(); ++i)
            values_[i].store(specs()[i].defaultValue, std::memory_order_relaxed);
        if constexpr (kSamples)
            files_.resize(static_cast<std::size_t>(std::max(effect_.sampleSlotCount(), 0)));
    }

    /// What info() gives, without making one.
    [[nodiscard]] static PluginInfo describe(const Extras& extras = {})
    {
        const dsp::ProcessorSpec spec = Effect::spec();
        PluginInfo info{spec.id, spec.name, "Vloom Audio", spec.version,
            kInstrument ? PluginInfo::Kind::Instrument : PluginInfo::Kind::Effect};
        info.category = extras.category;
        return info;
    }

    [[nodiscard]] PluginInfo info() const override { return describe(extras_); }

    [[nodiscard]] std::vector<ParameterInfo> parameters() const override
    {
        std::vector<ParameterInfo> list;
        list.reserve(specs().size());
        for (const dsp::ParameterSpec& p : specs()) {
            ParameterInfo info{
                p.id, p.name, p.unit, p.minValue, p.maxValue, p.defaultValue, p.stepped, p.group};
            if (p.stepped && extras_.labels != nullptr) {
                info.valueLabels = extras_.labels(p.id);
                // One per whole value, or none: a partial list would misname them.
                const double values = p.maxValue - p.minValue + 1.0;
                if (static_cast<double>(info.valueLabels.size()) != values)
                    info.valueLabels.clear();
            }
            list.push_back(std::move(info));
        }
        return list;
    }

    [[nodiscard]] bool hasSidechain() const override { return dsp::SidechainEffect<Effect>; }

    [[nodiscard]] std::optional<float> gainReductionDb() const override
    {
        if constexpr (GainReductionMeter<Effect>)
            return static_cast<float>(effect_.gainReductionDb());
        else
            return std::nullopt;
    }

    [[nodiscard]] std::vector<ReadoutInfo> readouts() const override
    {
        std::vector<ReadoutInfo> list;
        list.reserve(kReadouts.size() + dspReadoutSpecs().size());
        for (const Readout<Effect>& readout : kReadouts)
            list.push_back(ReadoutInfo{readout.id, readout.name, readout.unit, readout.toggle});
        for (const dsp::ReadoutSpec& spec : dspReadoutSpecs())
            list.push_back(dspReadoutInfo(spec));
        return list;
    }

    [[nodiscard]] float readoutValue(int index) const noexcept override
    {
        if (index < 0)
            return 0.0f;
        const auto at = static_cast<std::size_t>(index);
        if constexpr (kReadoutCount<Effect> > 0) {
            if (at < kReadouts.size())
                return kReadouts[at].value(effect_);
        }
        if constexpr (dsp::HasReadouts<Effect>) {
            const std::size_t past = at - kReadouts.size();
            if (at >= kReadouts.size() && past < dspReadoutSpecs().size())
                return effect_.readout(past); // lock-free (SharedValue), any thread
        }
        (void)at; // unused without readouts
        return 0.0f;
    }

    bool activate(double sampleRate, int maxBlockFrames) override
    {
        if (!(sampleRate > 0.0) || !std::isfinite(sampleRate) || maxBlockFrames <= 0)
            return false;
        // Not processing now (ADR 0006): the effect is ours to set up. The
        // sample slots must not change while it prepares.
        const std::lock_guard lock(slotsMutex_);
        pending_.store(false, std::memory_order_relaxed);
        applyValues();
        effect_.prepare(sampleRate, maxBlockFrames); // starts at the values, no glide
        tempo_ = 0.0; // the first block sets it
        latency_.store(std::max(effect_.latencyFrames(), 0), std::memory_order_relaxed);
        return true;
    }

    /// As of activation or the last parameter change the audio thread applied.
    [[nodiscard]] int latencyFrames() const override { return latency_.load(std::memory_order_relaxed); }

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
        if constexpr (kSamples) {
            std::vector<std::string> paths;
            {
                const std::lock_guard lock(slotsMutex_);
                paths.reserve(files_.size());
                for (const SlotFile& file : files_)
                    paths.push_back(file.path);
            }
            appendSamplePaths(state, paths);
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
        const std::size_t entryBytes = static_cast<std::size_t>(count) * 12;
        // The parameters, then (built-ins with sample slots) their paths.
        if (entries.size() < entryBytes || (!kSamples && entries.size() != entryBytes))
            return false;
        std::vector<std::string> paths;
        if constexpr (kSamples) {
            const std::span<const std::uint8_t> rest = entries.subspan(entryBytes);
            const int slots = static_cast<int>(files_.size());
            if (!rest.empty() && !readSamplePaths(rest, slots, paths))
                return false;
            paths.resize(files_.size()); // none listed: every slot empty
        }
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
        if constexpr (kSamples) {
            // A file that cannot be read keeps its path and says why; the
            // rest of the state still loads.
            for (std::size_t slot = 0; slot < paths.size(); ++slot) {
                if (paths[slot].empty()) {
                    (void)clearSample(static_cast<int>(slot));
                    continue;
                }
                std::string error;
                if (!loadSample(static_cast<int>(slot), paths[slot], &error))
                    forget(static_cast<int>(slot), paths[slot], error);
            }
        }
        return true;
    }

    void process(const ProcessContext& context) noexcept override
    {
        if (pending_.exchange(false, std::memory_order_acquire)) {
            applyValues();
            updateLatency();
        }
        if constexpr (dsp::TempoAware<Effect>) {
            const double tempo = context.transport.tempo;
            if (tempo != tempo_ && std::isfinite(tempo) && tempo > 0.0) {
                tempo_ = tempo;
                effect_.setTempo(tempo);
            }
        }
        const int numChannels = std::clamp(context.numChannels, 0, kMaxChannels);
        const int numFrames = std::max(context.numFrames, 0);
        const std::span<const ParameterEvent> changes = context.parameters;
        const std::span<const NoteEvent> notes = kInstrument ? context.notes : std::span<const NoteEvent>();
        const float* const* key = dsp::SidechainEffect<Effect> ? context.sidechain : nullptr;
        int position = 0;
        std::size_t change = 0;
        std::size_t note = 0;
        bool changed = false;
        // Both lists are in frame order: take whichever comes first.
        while (change < changes.size() || note < notes.size()) {
            constexpr int kNever = std::numeric_limits<int>::max();
            const int changeFrame
                = change < changes.size() ? std::clamp(changes[change].frame, position, numFrames) : kNever;
            const int noteFrame
                = note < notes.size() ? std::clamp(notes[note].frame, position, numFrames) : kNever;
            const int frame = std::min(changeFrame, noteFrame);
            run(context.channels, key, numChannels, position, frame - position);
            position = frame;
            if (changeFrame <= noteFrame) {
                const ParameterEvent& event = changes[change++];
                const int index = indexOf(event.id);
                if (index < 0 || !std::isfinite(event.value))
                    continue;
                const double value = clampTo(specs()[static_cast<std::size_t>(index)], event.value);
                values_[static_cast<std::size_t>(index)].store(value, std::memory_order_relaxed);
                effect_.setParameter(event.id, value);
                changed = true;
            } else {
                play(notes[note++]);
            }
        }
        run(context.channels, key, numChannels, position, numFrames - position);
        if (changed)
            updateLatency();
    }

    void reset() noexcept override { effect_.reset(); }

    /// Releases the samples replaced earlier that the audio thread let go of.
    void idle() override
    {
        if constexpr (kSamples) {
            const std::lock_guard lock(slotsMutex_);
            effect_.collectGarbage();
        }
    }

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

    // ---- sample slots (dsp::SampleSlots): host side, see AudioProcessor

    [[nodiscard]] int sampleSlotCount() const override { return static_cast<int>(files_.size()); }

    [[nodiscard]] SampleSlotInfo sampleSlot(int slot) const override
    {
        SampleSlotInfo info;
        if constexpr (kSamples) {
            if (!validSlot(slot))
                return info;
            info.name = files_.size() == 1 ? std::string("Sample") : "Pad " + std::to_string(slot + 1);
            const std::lock_guard lock(slotsMutex_);
            const SlotFile& file = files_[static_cast<std::size_t>(slot)];
            info.path = file.path;
            info.error = file.error;
            const dsp::SampleInfo playing = effect_.sampleInfo(slot);
            info.numChannels = playing.numChannels;
            info.frames = playing.frames;
            info.sampleRate = playing.sampleRate;
        } else {
            (void)slot;
        }
        return info;
    }

    bool loadSample(int slot, const std::string& path, std::string* error = nullptr) override
    {
        if constexpr (kSamples) {
            if (!validSlot(slot)) {
                if (error != nullptr)
                    *error = "no sample slot " + std::to_string(slot + 1);
                return false;
            }
            SampleFileResolver resolver;
            {
                const std::lock_guard lock(slotsMutex_);
                resolver = resolver_;
            }
            // Decoded outside the lock: a long file must not hold up idle().
            std::shared_ptr<const dsp::SampleBuffer> buffer = readSampleFile(path, resolver, error);
            if (buffer == nullptr)
                return false;
            try {
                // The path is copied first and stored only once the sample
                // is in: if anything throws, the slot keeps both what it
                // played and the path it saves.
                std::string newPath = path;
                const std::lock_guard lock(slotsMutex_);
                effect_.setSample(slot, std::move(buffer));
                SlotFile& file = files_[static_cast<std::size_t>(slot)];
                file.path = std::move(newPath);
                file.error = std::string{};
                slotsVersion_.fetch_add(1, std::memory_order_release);
            } catch (const std::exception&) {
                if (error != nullptr)
                    *error = "Not enough memory to load the sample";
                return false;
            }
            return true;
        } else {
            return AudioProcessor::loadSample(slot, path, error);
        }
    }

    bool clearSample(int slot) override
    {
        if constexpr (kSamples) {
            if (!validSlot(slot))
                return false;
            const std::lock_guard lock(slotsMutex_);
            try {
                effect_.setSample(slot, nullptr);
            } catch (const std::exception&) {
                return false; // could not retire the old one: it keeps playing, and its path stays
            }
            files_[static_cast<std::size_t>(slot)] = SlotFile{};
            slotsVersion_.fetch_add(1, std::memory_order_release);
            return true;
        } else {
            (void)slot;
            return false;
        }
    }

    [[nodiscard]] std::shared_ptr<const SampleAudio> sampleAudio(int slot) const override
    {
        if constexpr (kSamples) {
            if (!validSlot(slot))
                return nullptr;
            std::shared_ptr<const dsp::SampleBuffer> buffer;
            {
                // sample() is host side: serialized with setSample().
                const std::lock_guard lock(slotsMutex_);
                buffer = effect_.sample(slot);
            }
            return sampleAudioOf(std::move(buffer));
        } else {
            (void)slot;
            return nullptr;
        }
    }

    [[nodiscard]] std::uint64_t sampleSlotsVersion() const noexcept override
    {
        return slotsVersion_.load(std::memory_order_acquire);
    }

    void setSampleFileResolver(SampleFileResolver resolver) override
    {
        const std::lock_guard lock(slotsMutex_);
        resolver_ = std::move(resolver);
    }

private:
    static constexpr int kMaxChannels = 8;
    static constexpr int kMaxKeyChannels = 2;
    static constexpr bool kInstrument = dsp::Instrument<Effect>;
    static constexpr bool kSamples = dsp::SampleSlots<Effect>;

    struct SlotFile {
        std::string path;
        std::string error;
    };

    static constexpr auto kReadouts = readoutTable<Effect>();

    [[nodiscard]] bool validSlot(int slot) const noexcept
    {
        return slot >= 0 && static_cast<std::size_t>(slot) < files_.size();
    }

    /// The slot plays nothing, but keeps `path` and why it could not be read.
    void forget(int slot, const std::string& path, const std::string& error)
    {
        if constexpr (kSamples) {
            const std::lock_guard lock(slotsMutex_);
            files_[static_cast<std::size_t>(slot)]
                = SlotFile{path, error.empty() ? "Cannot read the file" : error};
            try {
                effect_.setSample(slot, nullptr);
            } catch (const std::exception&) {
            }
            slotsVersion_.fetch_add(1, std::memory_order_release);
        } else {
            (void)slot;
            (void)path;
            (void)error;
        }
    }

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

    /// vloom-dsp's readouts (HasReadouts), listed after the table's; empty without.
    [[nodiscard]] static std::span<const dsp::ReadoutSpec> dspReadoutSpecs() noexcept
    {
        if constexpr (dsp::HasReadouts<Effect>)
            return Effect::readoutSpecs();
        else
            return {};
    }

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

    /// Audio thread, after parameter changes: the latency some of them change.
    void updateLatency() noexcept
    {
        latency_.store(std::max(effect_.latencyFrames(), 0), std::memory_order_relaxed);
    }

    void run(
        float* const* channels, const float* const* key, int numChannels, int offset, int frames) noexcept
    {
        if (frames <= 0 || channels == nullptr)
            return;
        std::array<float*, kMaxChannels> part{};
        for (int ch = 0; ch < numChannels; ++ch)
            part[static_cast<std::size_t>(ch)] = channels[ch] + offset;
        if constexpr (dsp::SidechainEffect<Effect>) {
            if (key != nullptr && key[0] != nullptr && key[1] != nullptr) {
                const std::array<const float*, kMaxKeyChannels> keyPart{key[0] + offset, key[1] + offset};
                effect_.processWithSidechain(
                    part.data(), keyPart.data(), numChannels, kMaxKeyChannels, frames);
                return;
            }
        } else {
            (void)key;
        }
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

    Extras extras_;
    Effect effect_; ///< audio thread, once active (constructed in place: some cannot move)
    std::unique_ptr<std::atomic<double>[]> values_; ///< per parameter, in specs() order
    std::atomic<bool> pending_{false}; ///< values set off the audio thread wait for it
    std::atomic<int> latency_{0}; ///< audio thread writes after changes, control thread reads
    double tempo_ = 0.0; ///< audio thread: the tempo last given (0: none yet)

    // Sample slots, host side. The mutex serializes the host side of the
    // effect's slots (setSample(), collectGarbage(), prepare()) and guards
    // the files; the audio thread never takes it.
    mutable std::mutex slotsMutex_;
    std::vector<SlotFile> files_; ///< one per slot; sized once
    SampleFileResolver resolver_;
    std::atomic<std::uint64_t> slotsVersion_{0}; ///< sampleSlotsVersion(): bumped under the mutex
};

} // namespace vloom::builtin
