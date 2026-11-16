// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/Vst2Host.hpp"

#include "plugin/PluginFiles.hpp"
#include "plugin/Vst2Abi.hpp"
#include "plugin/Vst2Bridge.hpp"
#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/plugin/PluginInstance.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>

namespace vloom {

namespace {

using plugin_files::fail;

/// A library symbol as a function pointer (without a cast between object
/// and function pointers, which ISO C++ leaves to the implementation).
template <typename Function> Function functionAt(void* symbol) noexcept
{
    static_assert(sizeof(Function) == sizeof(void*));
    Function f = nullptr;
    std::memcpy(&f, &symbol, sizeof(f));
    return f;
}

/// The binary of a plugin: the file itself, or on macOS the executable of
/// the .vst bundle.
std::filesystem::path binaryOf(const std::filesystem::path& path)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return path / "Contents" / "MacOS" / path.stem();
    return path;
}

/// A VST 2 plugin as an AudioProcessor. The control thread is the plugin's
/// UI thread; process() and reset() run on the audio thread.
class Vst2Processor final : public AudioProcessor {
public:
    static constexpr int kMaxChannels = 256; ///< per direction; this only bounds the memory
    static constexpr int kMaxParameters = 65536;
    static constexpr std::size_t kMaxEvents = 512;
    static constexpr std::intptr_t kMaxStateBytes = std::intptr_t{1} << 30; ///< 1 GiB

    Vst2Processor(std::shared_ptr<Vst2Library> library, std::string fallbackName)
        : library_(std::move(library))
        , mainThread_(std::this_thread::get_id())
    {
        info_.name = std::move(fallbackName);
        for (std::size_t i = 0; i < kMaxEvents; ++i) {
            midi_[i] = {};
            midi_[i].type = vst2::kMidiEventType;
            midi_[i].byteSize = static_cast<std::int32_t>(sizeof(vst2::MidiEvent));
            events_.pointers[i] = &midi_[i];
        }
    }

    ~Vst2Processor() override
    {
        if (effect_ == nullptr)
            return;
        Vst2Processor::closeEditor();
        Vst2Processor::deactivate();
        dispatch(vst2::kClose); // the plugin frees itself
        effect_ = nullptr;
    }

    bool create(vst2::EntryPoint entry, std::string* error)
    {
        // The plugin may call the host before it has handed itself over.
        creating_ = this;
        vst2::Effect* effect = entry(&Vst2Processor::hostCallback);
        creating_ = nullptr;
        if (effect == nullptr || effect->magic != vst2::kMagic || effect->dispatcher == nullptr) {
            fail(error, "the plugin could not be created");
            return false;
        }
        effect_ = effect;
        effect_->hostData2 = reinterpret_cast<std::intptr_t>(this);
        dispatch(vst2::kOpen);
        if (dispatch(vst2::kGetPlugCategory) == vst2::kCategoryShell) {
            fail(error, "the file is a shell of many plugins, which is not supported");
            return false;
        }
        if ((effect_->flags & vst2::kCanReplacing) == 0 || effect_->processReplacing == nullptr
            || effect_->setParameter == nullptr || effect_->getParameter == nullptr) {
            fail(error, "the plugin cannot process 32-bit audio");
            return false;
        }
        if (const std::string name = text(vst2::kGetEffectName); !name.empty())
            info_.name = name;
        else if (const std::string product = text(vst2::kGetProductString); !product.empty())
            info_.name = product;
        info_.vendor = text(vst2::kGetVendorString);
        info_.version = std::to_string(effect_->version);
        info_.id = plugin_files::hex8(static_cast<std::uint32_t>(effect_->uniqueId));
        info_.kind = (effect_->flags & vst2::kIsSynth) != 0 ? PluginInfo::Kind::Instrument
                                                            : PluginInfo::Kind::Effect;
        std::array<char, 32> sends{"sendVstMidiEvent"};
        makesNotes_ = dispatch(vst2::kCanDo, 0, 0, sends.data()) == 1;
        // The values it starts with are the nearest thing to defaults.
        const int count = parameterCount();
        defaults_.resize(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
            defaults_[static_cast<std::size_t>(i)] = effect_->getParameter(effect_, i);
        return true;
    }

    [[nodiscard]] PluginInfo info() const override { return info_; }

    [[nodiscard]] std::vector<ParameterInfo> parameters() const override
    {
        std::vector<ParameterInfo> list;
        const int count = std::min(parameterCount(), static_cast<int>(defaults_.size()));
        list.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            ParameterInfo parameter;
            parameter.id = static_cast<std::uint32_t>(i);
            parameter.name = text(vst2::kGetParamName, i);
            if (parameter.name.empty())
                parameter.name = "Parameter " + std::to_string(i + 1);
            parameter.unit = text(vst2::kGetParamLabel, i);
            parameter.minValue = 0.0;
            parameter.maxValue = 1.0;
            const float value = defaults_[static_cast<std::size_t>(i)];
            parameter.defaultValue
                = std::isfinite(value) ? std::clamp(static_cast<double>(value), 0.0, 1.0) : 0.0;
            list.push_back(std::move(parameter));
        }
        return list;
    }

    bool activate(double sampleRate, int maxBlockFrames) override
    {
        deactivate();
        const int ins = effect_->numInputs;
        const int outs = effect_->numOutputs;
        if (maxBlockFrames <= 0 || ins < 0 || outs < 0 || ins > kMaxChannels || outs > kMaxChannels)
            return false;
        const auto size = [maxBlockFrames](int count, std::vector<std::vector<float>>& channels,
                              std::vector<float*>& pointers) {
            channels.assign(static_cast<std::size_t>(count),
                std::vector<float>(static_cast<std::size_t>(maxBlockFrames)));
            pointers.clear();
            for (auto& channel : channels)
                pointers.push_back(channel.data());
        };
        size(ins, inputs_, inputPointers_);
        size(outs, outputs_, outputPointers_);
        sampleRate_ = sampleRate;
        blockFrames_ = maxBlockFrames;
        time_ = {};
        time_.sampleRate = sampleRate;
        time_.tempo = 120.0;
        dispatch(vst2::kSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate));
        dispatch(vst2::kSetBlockSize, 0, maxBlockFrames);
        dispatch(vst2::kMainsChanged, 0, 1);
        dispatch(vst2::kStartProcess);
        held_.fill(0);
        resetPending_ = false;
        active_ = true;
        return true;
    }

    void deactivate() override
    {
        if (!active_)
            return;
        // The host calls this only once the audio thread no longer
        // processes the plugin (ADR 0006).
        dispatch(vst2::kStopProcess);
        dispatch(vst2::kMainsChanged, 0, 0);
        active_ = false;
    }

    [[nodiscard]] std::optional<double> parameterValue(std::uint32_t id) const override
    {
        if (id >= static_cast<std::uint32_t>(parameterCount()))
            return std::nullopt;
        const float value = effect_->getParameter(effect_, static_cast<std::int32_t>(id));
        return std::isfinite(value) ? std::optional<double>(std::clamp(static_cast<double>(value), 0.0, 1.0))
                                    : std::nullopt;
    }

    [[nodiscard]] int extraOutputCount() const override
    {
        return std::min(std::max(static_cast<int>(outputs_.size()) - 2, 0) / 2,
            static_cast<int>(PluginInstance::kMaxExtraOutputs));
    }
    [[nodiscard]] bool hasSidechain() const override { return inputs_.size() >= 4; }
    [[nodiscard]] bool makesNotes() const override { return makesNotes_; }

    [[nodiscard]] std::vector<std::string> programs() const override
    {
        std::vector<std::string> names;
        const int count = programCount();
        // A plugin with one program has nothing to choose from.
        for (int i = 0; count > 1 && i < count; ++i) {
            names.push_back(text(vst2::kGetProgramNameIndexed, i));
            if (names.back().empty())
                names.back() = "Program " + std::to_string(i + 1);
        }
        return names;
    }
    [[nodiscard]] int currentProgram() const override
    {
        const int count = programCount();
        const std::intptr_t program = dispatch(vst2::kGetProgram);
        return count > 1 && program >= 0 && program < count ? static_cast<int>(program) : -1;
    }
    bool setProgram(int index) override
    {
        if (index < 0 || index >= programCount())
            return false;
        dispatch(vst2::kSetProgram, 0, index);
        return true;
    }

    [[nodiscard]] int latencyFrames() const override
    {
        return active_ ? std::clamp(effect_->initialDelay, 0, 1 << 30) : 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> saveState() const override
    {
        // "VLV2", then 'C' and the plugin's chunk, or 'P' and its parameter
        // values (floats as the machine has them).
        std::vector<std::uint8_t> out{'V', 'L', 'V', '2'};
        if ((effect_->flags & vst2::kProgramChunks) != 0) {
            void* data = nullptr;
            const std::intptr_t size = dispatch(vst2::kGetChunk, 0, 0, &data);
            if (data == nullptr || size <= 0 || size > kMaxStateBytes)
                return {};
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            out.push_back('C');
            out.insert(out.end(), bytes, bytes + size);
            return out;
        }
        out.push_back('P');
        const int count = parameterCount();
        for (int i = 0; i < count; ++i) {
            const float value = effect_->getParameter(effect_, i);
            std::array<std::uint8_t, sizeof(float)> bytes{};
            std::memcpy(bytes.data(), &value, sizeof(float));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        return out;
    }

    bool loadState(std::span<const std::uint8_t> data) override
    {
        if (data.size() < 5 || std::memcmp(data.data(), "VLV2", 4) != 0)
            return false;
        const std::span<const std::uint8_t> body = data.subspan(5);
        if (data[4] == 'C') {
            if ((effect_->flags & vst2::kProgramChunks) == 0 || body.empty()
                || body.size() > static_cast<std::size_t>(kMaxStateBytes))
                return false;
            // The plugin only reads it; the call takes a plain pointer.
            std::vector<std::uint8_t> copy(body.begin(), body.end());
            dispatch(vst2::kSetChunk, 0, static_cast<std::intptr_t>(copy.size()), copy.data());
            return true;
        }
        if (data[4] != 'P' || body.size() % sizeof(float) != 0)
            return false;
        const int count = std::min(parameterCount(), static_cast<int>(body.size() / sizeof(float)));
        for (int i = 0; i < count; ++i) {
            float value = 0.0f;
            std::memcpy(&value, body.data() + static_cast<std::size_t>(i) * sizeof(float), sizeof(float));
            if (std::isfinite(value))
                effect_->setParameter(effect_, i, std::clamp(value, 0.0f, 1.0f));
        }
        return true;
    }

    void process(const ProcessContext& context) noexcept override
    {
        if (!active_ || context.numFrames <= 0 || context.numFrames > blockFrames_ || context.numChannels < 1)
            return;
        // A plugin that changed its channels since activation would write
        // past the buffers: left out until it is activated again.
        if (effect_->numInputs != static_cast<int>(inputs_.size())
            || effect_->numOutputs != static_cast<int>(outputs_.size()))
            return;
        audioThread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        const auto frames = static_cast<std::size_t>(context.numFrames);
        float* left = context.channels[0];
        float* right = context.numChannels > 1 ? context.channels[1] : context.channels[0];

        // Parameters: VST 2 takes them between blocks, not at a frame.
        for (const ParameterEvent& p : context.parameters)
            if (p.id < static_cast<std::uint32_t>(parameterCount()) && std::isfinite(p.value))
                effect_->setParameter(effect_, static_cast<std::int32_t>(p.id),
                    static_cast<float>(std::clamp(p.value, 0.0, 1.0)));

        // Notes as MIDI. After a reset, every held key ends first.
        std::size_t count = 0;
        const auto note = [&](int frame, bool on, int key, float velocity, int channel) {
            if (count == kMaxEvents)
                return;
            vst2::MidiEvent& e = midi_[count++];
            e.deltaFrames = std::clamp(frame, 0, context.numFrames - 1);
            e.midiData[0] = static_cast<char>((on ? 0x90 : 0x80) | channel);
            e.midiData[1] = static_cast<char>(key);
            // A note-on at velocity 0 would mean note-off.
            e.midiData[2]
                = static_cast<char>(on ? std::clamp<long>(std::lround(velocity * 127.0f), 1, 127) : 0);
            e.midiData[3] = 0;
            std::uint16_t& channels = held_[static_cast<std::size_t>(key)];
            channels
                = static_cast<std::uint16_t>(on ? channels | (1u << channel) : channels & ~(1u << channel));
        };
        // Controls as MIDI, before the notes (they come at the block's start).
        for (const ControlEvent& control : context.controls) {
            // Room is kept for the note-offs after a reset and for the block's notes.
            if (count + 128 + PluginInstance::kMaxNotes >= kMaxEvents || !std::isfinite(control.value))
                break;
            vst2::MidiEvent& e = midi_[count++];
            e.deltaFrames = std::clamp(control.frame, 0, context.numFrames - 1);
            const int controlChannel = std::clamp<int>(control.channel, 0, 15);
            const auto seven = [](float value) {
                return static_cast<char>(std::clamp<long>(std::lround(value * 127.0f), 0, 127));
            };
            if (control.type == ControlEvent::Type::PitchBend) {
                const long bend
                    = std::clamp<long>(std::lround((control.value + 1.0f) * 0.5f * 16383.0f), 0, 16383);
                e.midiData[0] = static_cast<char>(0xE0 | controlChannel);
                e.midiData[1] = static_cast<char>(bend & 0x7F);
                e.midiData[2] = static_cast<char>(bend >> 7);
            } else if (control.type == ControlEvent::Type::Controller) {
                e.midiData[0] = static_cast<char>(0xB0 | controlChannel);
                e.midiData[1] = static_cast<char>(std::clamp<int>(control.number, 0, 127));
                e.midiData[2] = seven(control.value);
            } else {
                e.midiData[0] = static_cast<char>(0xD0 | controlChannel);
                e.midiData[1] = seven(control.value);
                e.midiData[2] = 0;
            }
            e.midiData[3] = 0;
        }
        if (resetPending_.exchange(false, std::memory_order_relaxed))
            for (int key = 0; key < 128; ++key)
                for (int channel = 0; channel < 16; ++channel)
                    if (((held_[static_cast<std::size_t>(key)] >> channel) & 1u) != 0)
                        note(0, false, key, 0.0f, channel);
        for (const NoteEvent& n : context.notes)
            if (n.key >= 0 && n.key < 128)
                note(n.frame, n.type == NoteEvent::Type::On, n.key, std::clamp(n.velocity, 0.0f, 1.0f),
                    std::clamp<int>(n.channel, 0, 15));

        // The song, for a plugin that asks during the block.
        const TransportInfo& song = context.transport;
        time_.sampleRate = sampleRate_;
        time_.tempo = song.tempo;
        time_.ppqPos = song.positionTicks / kPpq;
        time_.samplePos = song.tempo > 0.0 ? time_.ppqPos * 60.0 / song.tempo * sampleRate_ : 0.0;
        time_.flags = vst2::kPpqPosValid | vst2::kTempoValid | (song.playing ? vst2::kTransportPlaying : 0);
        if (song.beatsPerBar > 0 && song.beatUnit > 0) {
            const double barBeats = song.beatsPerBar * 4.0 / song.beatUnit;
            time_.timeSigNumerator = song.beatsPerBar;
            time_.timeSigDenominator = song.beatUnit;
            time_.barStartPos = std::floor(time_.ppqPos / barBeats) * barBeats;
            time_.flags |= vst2::kTimeSigValid | vst2::kBarsValid;
        }
        if (song.loopEndTicks > song.loopStartTicks) {
            time_.cycleStartPos = song.loopStartTicks / kPpq;
            time_.cycleEndPos = song.loopEndTicks / kPpq;
            time_.flags |= vst2::kCyclePosValid | vst2::kTransportCycleActive;
        }

        // Audio: ours into the first two inputs; every other buffer silent.
        for (std::size_t ch = 0; ch < inputs_.size(); ++ch) {
            float* dst = inputPointers_[ch];
            if (inputs_.size() == 1)
                for (std::size_t i = 0; i < frames; ++i)
                    dst[i] = 0.5f * (left[i] + right[i]);
            else if (ch < 2)
                std::copy_n(ch == 0 ? left : right, frames, dst);
            else if (ch < 4 && context.sidechain != nullptr)
                std::copy_n(context.sidechain[ch - 2], frames, dst); // inputs 3 and 4: the sidechain
            else
                std::fill_n(dst, frames, 0.0f);
        }
        for (float* dst : outputPointers_)
            std::fill_n(dst, frames, 0.0f);

        madeCount_ = 0;
        if (count > 0) {
            events_.numEvents = static_cast<std::int32_t>(count);
            dispatch(vst2::kProcessEvents, 0, 0, &events_);
        }
        effect_->processReplacing(effect_, inputPointers_.data(), outputPointers_.data(), context.numFrames);
        audioThread_.store(std::thread::id{}, std::memory_order_relaxed);
        // The notes it sent the host during the block (an arpeggiator).
        if (context.notesOutCount != nullptr) {
            const std::size_t made = std::min(madeCount_, context.notesOut.size());
            for (std::size_t i = 0; i < made; ++i) {
                context.notesOut[i] = made_[i];
                context.notesOut[i].frame = std::clamp(made_[i].frame, 0, context.numFrames - 1);
            }
            *context.notesOutCount = static_cast<int>(made);
        }
        // Its outputs after the first two, in pairs, for the host to route apart.
        for (std::size_t k = 0; k + 1 < context.extraOutputs.size(); k += 2) {
            if (k + 3 >= outputPointers_.size())
                break; // no such pair: the host's silence stays
            std::copy_n(outputPointers_[k + 2], frames, context.extraOutputs[k]);
            std::copy_n(outputPointers_[k + 3], frames, context.extraOutputs[k + 1]);
        }
        if (outputs_.empty())
            return; // the audio stays as it came

        // The first two outputs back. A plugin with no audio input (an
        // instrument) adds to what is there; an effect replaces it.
        const float* outLeft = outputPointers_[0];
        const float* outRight = outputPointers_.size() > 1 ? outputPointers_[1] : outputPointers_[0];
        const bool add = inputs_.empty();
        for (std::size_t i = 0; i < frames; ++i) {
            left[i] = add ? left[i] + outLeft[i] : outLeft[i];
            if (right != left)
                right[i] = add ? right[i] + outRight[i] : outRight[i];
        }
    }

    void reset() noexcept override
    {
        // VST 2 has no reset: end every held note in the next block.
        resetPending_.store(true, std::memory_order_relaxed);
    }

    void idle() override
    {
        if (editorOpen_)
            dispatch(vst2::kEditIdle);
        Edit edit;
        while (edits_.pop(edit))
            if (listener_ != nullptr)
                listener_->parameterEdited(edit.id, edit.value);
    }

    [[nodiscard]] bool hasEditor() const override
    {
#if defined(__linux__)
        return false; // X11 editors need the host's display connection, which the engine has none of
#else
        return (effect_->flags & vst2::kHasEditor) != 0;
#endif
    }

    bool openEditor(void* parent, EditorListener* listener) override
    {
        closeEditor();
        if (parent == nullptr || !hasEditor())
            return false;
        listener_ = listener; // the editor may ask for a size while it opens
        editorOpen_ = true;
        if (dispatch(vst2::kEditOpen, 0, 0, parent) == 0) {
            editorOpen_ = false;
            listener_ = nullptr;
            return false;
        }
        return true;
    }

    void closeEditor() override
    {
        if (!editorOpen_)
            return;
        dispatch(vst2::kEditClose);
        editorOpen_ = false;
        listener_ = nullptr;
    }

    [[nodiscard]] bool isEditorOpen() const override { return editorOpen_; }

    [[nodiscard]] EditorSize editorSize() const override
    {
        vst2::Rect* rect = nullptr;
        if (!editorOpen_ || dispatch(vst2::kEditGetRect, 0, 0, &rect) == 0 || rect == nullptr)
            return {};
        return {std::max(rect->right - rect->left, 0), std::max(rect->bottom - rect->top, 0)};
    }

private:
    /// A parameter edited in the editor, reported from another thread than
    /// the control thread.
    struct Edit {
        std::uint32_t id = 0;
        double value = 0.0;
    };

    /// The header of vst2::Events with room for a block's events.
    struct EventBlock {
        std::int32_t numEvents = 0;
        std::intptr_t reserved = 0;
        std::array<vst2::MidiEvent*, kMaxEvents> pointers{};
    };

    [[nodiscard]] int parameterCount() const noexcept
    {
        return std::clamp(effect_->numParams, 0, kMaxParameters);
    }

    std::intptr_t dispatch(std::int32_t opcode, std::int32_t index = 0, std::intptr_t value = 0,
        void* ptr = nullptr, float opt = 0.0f) const noexcept
    {
        return effect_->dispatcher(effect_, opcode, index, value, ptr, opt);
    }

    /// A string the plugin writes for `opcode`. The interface names sizes of
    /// 8 to 64 characters and plugins write more: the buffer has room.
    [[nodiscard]] std::string text(std::int32_t opcode, std::int32_t index = 0) const
    {
        std::array<char, 512> buffer{};
        dispatch(opcode, index, 0, buffer.data());
        buffer.back() = '\0';
        return plugin_files::printable(
            std::string(buffer.begin(), std::find(buffer.begin(), buffer.end(), char{})));
    }

    static void copyText(void* ptr, const char* value) noexcept
    {
        if (ptr != nullptr)
            std::memcpy(ptr, value, std::strlen(value) + 1); // room for 64 characters, by the interface
    }

    std::intptr_t fromPlugin(
        std::int32_t opcode, std::int32_t index, std::intptr_t value, void* ptr, float opt)
    {
        switch (opcode) {
        case vst2::kHostAutomate: {
            // An edit in the editor; the plugin has taken the value itself.
            if (index < 0 || index >= parameterCount() || !std::isfinite(opt))
                return 0;
            const Edit edit{
                static_cast<std::uint32_t>(index), std::clamp(static_cast<double>(opt), 0.0, 1.0)};
            if (std::this_thread::get_id() != mainThread_) {
                // The queue takes one writer at a time, and a plugin may
                // report from more than one of its threads: the second one
                // in is dropped, as when the queue is full (the editor
                // still shows the value).
                if (!editsBusy_.test_and_set(std::memory_order_acquire)) {
                    edits_.push(edit);
                    editsBusy_.clear(std::memory_order_release);
                }
            } else if (listener_ != nullptr)
                listener_->parameterEdited(edit.id, edit.value);
            return 0;
        }
        case vst2::kHostProcessEvents: {
            // Notes from the plugin, taken only while it processes a block.
            const auto* sent = static_cast<const vst2::Events*>(ptr);
            if (sent == nullptr || std::this_thread::get_id() != audioThread_.load(std::memory_order_relaxed))
                return 0;
            const std::int32_t events = std::clamp(sent->numEvents, 0, 1024);
            // The list goes on past the two entries the structure declares.
            const auto* list = reinterpret_cast<vst2::MidiEvent* const*>(
                reinterpret_cast<const char*>(sent) + offsetof(vst2::Events, events));
            for (std::int32_t i = 0; i < events && madeCount_ < made_.size(); ++i) {
                const vst2::MidiEvent* e = list[i];
                if (e == nullptr || e->type != vst2::kMidiEventType)
                    continue;
                const int status = static_cast<unsigned char>(e->midiData[0]) & 0xF0;
                const int key = static_cast<unsigned char>(e->midiData[1]) & 0x7F;
                const int velocity = static_cast<unsigned char>(e->midiData[2]) & 0x7F;
                if (status != 0x90 && status != 0x80)
                    continue;
                const bool on = status == 0x90 && velocity > 0;
                made_[madeCount_++]
                    = NoteEvent{e->deltaFrames, on ? NoteEvent::Type::On : NoteEvent::Type::Off,
                        static_cast<std::int16_t>(key), static_cast<float>(velocity) / 127.0f,
                        static_cast<std::int16_t>(static_cast<unsigned char>(e->midiData[0]) & 0x0F)};
            }
            return 1;
        }
        case vst2::kHostGetTime:
            return reinterpret_cast<std::intptr_t>(&time_);
        case vst2::kHostSizeWindow:
            if (!editorOpen_ || listener_ == nullptr || std::this_thread::get_id() != mainThread_)
                return 0;
            listener_->editorResizeRequested(
                {std::max(index, 0), static_cast<int>(std::clamp<std::intptr_t>(value, 0, 1 << 20))});
            return 1;
        case vst2::kHostGetSampleRate:
            return static_cast<std::intptr_t>(sampleRate_);
        case vst2::kHostGetBlockSize:
            return blockFrames_;
        case vst2::kHostGetCurrentProcessLevel:
            // 2: the audio thread, 1: the UI thread.
            return std::this_thread::get_id() == audioThread_.load(std::memory_order_relaxed) ? 2 : 1;
        case vst2::kHostGetVendorString:
            copyText(ptr, "Vloom Audio");
            return 1;
        case vst2::kHostGetProductString:
            copyText(ptr, "Vloom");
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
            // Idle, display updates, channel changes (taken at the next
            // activation) and everything this host does not offer.
            return 0;
        }
    }

    static std::intptr_t VLOOM_VST2_CALL hostCallback(vst2::Effect* effect, std::int32_t opcode,
        std::int32_t index, std::intptr_t value, void* ptr, float opt)
    {
        if (opcode == vst2::kHostVersion)
            return 2400;
        Vst2Processor* self
            = effect != nullptr ? reinterpret_cast<Vst2Processor*>(effect->hostData2) : nullptr;
        if (self == nullptr)
            self = creating_; // still inside its entry point
        if (self == nullptr || (self->effect_ == nullptr && opcode != vst2::kHostCurrentId))
            return 0; // before it has handed itself over: nothing to answer with
        if (opcode == vst2::kHostCurrentId)
            return 0; // no shell: no plugin to choose
        return self->fromPlugin(opcode, index, value, ptr, opt);
    }

    /// The processor whose plugin is inside its entry point, on this thread.
    static thread_local Vst2Processor* creating_;

    std::shared_ptr<Vst2Library> library_; ///< kept loaded while the plugin lives
    PluginInfo info_;
    vst2::Effect* effect_ = nullptr;
    std::vector<float> defaults_; ///< the parameters' values at creation
    std::thread::id mainThread_; ///< the thread that created the plugin
    std::atomic<std::thread::id> audioThread_{}; ///< while a block is processed

    bool editorOpen_ = false;
    EditorListener* listener_ = nullptr;
    SpscQueue<Edit, 256> edits_; ///< audio thread to control thread
    std::atomic_flag editsBusy_ = ATOMIC_FLAG_INIT; ///< a thread is writing to edits_

    bool active_ = false;
    double sampleRate_ = 48000.0;
    int blockFrames_ = 0;
    std::atomic<bool> resetPending_{false};
    std::array<std::uint16_t, 128> held_{}; ///< audio thread: per key, the channels it was told it is down on
    vst2::TimeInfo time_{}; ///< audio thread, as of the block being processed
    bool makesNotes_ = false; ///< it says it sends MIDI to the host
    std::array<NoteEvent, PluginInstance::kMaxNotes> made_{}; ///< audio thread: the notes it sent this block
    std::size_t madeCount_ = 0;

    [[nodiscard]] int programCount() const noexcept { return std::clamp(effect_->numPrograms, 0, 4096); }

    std::vector<std::vector<float>> inputs_, outputs_;
    std::vector<float*> inputPointers_, outputPointers_;
    std::array<vst2::MidiEvent, kMaxEvents> midi_{};
    EventBlock events_;
};

thread_local Vst2Processor* Vst2Processor::creating_ = nullptr;

std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}

/// The open libraries: those whose plugins run in this process, and those
/// whose plugins run in a bridge. The same file can be open both ways.
std::map<std::filesystem::path, std::weak_ptr<Vst2Library>>& cache(bool bridged)
{
    static std::array<std::map<std::filesystem::path, std::weak_ptr<Vst2Library>>, 2> c;
    return c[bridged ? 1 : 0];
}

std::shared_ptr<Vst2Library> cached(const std::filesystem::path& key, bool bridged)
{
    const auto it = cache(bridged).find(key);
    return it != cache(bridged).end() ? it->second.lock() : nullptr;
}

} // namespace

struct Vst2Library::Impl {
    std::filesystem::path path;
    plugin_files::SharedLibrary module;
    vst2::EntryPoint entry = nullptr;
    std::filesystem::path bridge; ///< not empty: the plugin runs in this bridge, not in this process
    std::unique_ptr<AudioProcessor> spare; ///< the instance open() made to see what the plugin is (bridged)
    std::vector<PluginInfo> plugins;
};

Vst2Library::Vst2Library(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

Vst2Library::~Vst2Library() = default;

std::shared_ptr<Vst2Library> Vst2Library::open(const std::filesystem::path& path, std::string* error)
{
    return open(path, {}, error);
}

std::shared_ptr<Vst2Library> Vst2Library::openBridged(
    const std::filesystem::path& path, const std::filesystem::path& bridge, std::string* error)
{
    if (bridge.empty()) {
        fail(error, "no plugin bridge was given");
        return nullptr;
    }
    return open(path, bridge, error);
}

std::shared_ptr<Vst2Library> Vst2Library::open(
    const std::filesystem::path& path, std::filesystem::path bridge, std::string* error)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path key = ec ? path : canonical;
    const std::lock_guard lock(cacheMutex());
    // Open already, the way that was asked for: that one.
    if (auto library = cached(key, !bridge.empty()))
        return library;

    auto impl = std::make_unique<Impl>();
    impl->path = key;
    if (!std::filesystem::exists(key, ec)) {
        fail(error, "no such file: " + plugin_files::utf8(key));
        return nullptr;
    }
    // A 32-bit plugin cannot be loaded into this process: it goes into the
    // 32-bit bridge, if the application came with one.
    if (bridge.empty() && vst2_bridge::needs32BitBridge(binaryOf(key))) {
        bridge = vst2_bridge::find32BitBridge();
        if (bridge.empty()) {
            fail(error,
                plugin_files::utf8(key)
                    + " is a 32-bit plugin, and vloom-plugin-bridge32.exe, which runs those, was not found");
            return nullptr;
        }
        if (auto library = cached(key, true))
            return library;
    }
    std::string reason;
    if (!bridge.empty()) {
        // What the plugin is: only an instance can tell.
        std::unique_ptr<AudioProcessor> probe = vst2_bridge::create(bridge, key, &reason);
        if (probe == nullptr) {
            fail(error, plugin_files::utf8(key) + ": " + reason);
            return nullptr;
        }
        impl->bridge = std::move(bridge);
        impl->plugins.push_back(probe->info());
        impl->spare = std::move(probe); // the first create() takes it: the bridge is not started twice
        std::shared_ptr<Vst2Library> bridged(new Vst2Library(std::move(impl)));
        cache(true)[key] = bridged;
        return bridged;
    }
#if defined(_WIN32)
    // Any DLL may lie in a plugin folder, and loading one runs its code.
    // The file's own list of what it exports tells a plugin from the rest
    // without that.
    if (!plugin_files::readModule(binaryOf(key), {"VSTPluginMain", "main"}).exports) {
        fail(error, plugin_files::utf8(key) + " is not a VST 2 plugin (no VSTPluginMain)");
        return nullptr;
    }
#endif
    if (!impl->module.load(binaryOf(key), error))
        return nullptr;
    for (const char* name : {"VSTPluginMain", "main_macho", "main"}) {
        impl->entry = functionAt<vst2::EntryPoint>(impl->module.symbol(name));
        if (impl->entry != nullptr)
            break;
    }
    if (impl->entry == nullptr) {
        fail(error, plugin_files::utf8(key) + " is not a VST 2 plugin (no VSTPluginMain)");
        return nullptr;
    }
    std::shared_ptr<Vst2Library> library(new Vst2Library(std::move(impl)));
    const std::string stem = plugin_files::utf8(key.stem());
    {
        Vst2Processor probe(nullptr, stem);
        if (!probe.create(library->impl_->entry, &reason)) {
            fail(error, plugin_files::utf8(key) + ": " + reason);
            return nullptr;
        }
        library->impl_->plugins.push_back(probe.info());
    }
    cache(false)[key] = library;
    return library;
}

const std::filesystem::path& Vst2Library::path() const noexcept { return impl_->path; }

const std::vector<PluginInfo>& Vst2Library::plugins() const noexcept { return impl_->plugins; }

bool Vst2Library::isBridged() const noexcept { return !impl_->bridge.empty(); }

std::unique_ptr<AudioProcessor> Vst2Library::create(std::string_view id, std::string* error)
{
    if (impl_->plugins.empty() || impl_->plugins.front().id != id) {
        fail(error, "no plugin " + std::string(id) + " in " + plugin_files::utf8(impl_->path));
        return nullptr;
    }
    std::string reason;
    if (!impl_->bridge.empty()) {
        if (impl_->spare != nullptr && !impl_->spare->hasFailed())
            return std::move(impl_->spare);
        impl_->spare.reset();
        auto bridged = vst2_bridge::create(impl_->bridge, impl_->path, &reason);
        if (bridged == nullptr)
            fail(error, "the plugin " + impl_->plugins.front().name + " failed to start: " + reason);
        return bridged;
    }
    auto processor = std::make_unique<Vst2Processor>(shared_from_this(), impl_->plugins.front().name);
    if (!processor->create(impl_->entry, &reason)) {
        fail(error, "the plugin " + impl_->plugins.front().name + " failed to start: " + reason);
        return nullptr;
    }
    return processor;
}

std::vector<std::filesystem::path> vst2SearchPaths()
{
    std::vector<std::filesystem::path> paths = plugin_files::pathsFrom("VST_PATH");
    const auto environment = plugin_files::environment;
#if defined(_WIN32)
    // Where an installer was told the plugins go, if one wrote it down.
    std::array<wchar_t, 1024> folder{};
    DWORD bytes = static_cast<DWORD>((folder.size() - 1) * sizeof(wchar_t));
    // Both views of the registry: 32-bit installers write to their own.
    for (const DWORD view : {DWORD{0}, DWORD{RRF_SUBKEY_WOW6432KEY}}) {
        folder.fill(L'\0');
        bytes = static_cast<DWORD>((folder.size() - 1) * sizeof(wchar_t));
        if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VST", L"VSTPluginsPath", RRF_RT_REG_SZ | view,
                nullptr, folder.data(), &bytes)
                == ERROR_SUCCESS
            && folder[0] != L'\0')
            paths.emplace_back(folder.data());
    }
    if (const std::string common = environment("COMMONPROGRAMFILES"); !common.empty())
        paths.push_back(std::filesystem::path(common) / "VST2");
    if (const std::string programs = environment("PROGRAMFILES"); !programs.empty()) {
        paths.push_back(std::filesystem::path(programs) / "VSTPlugins");
        paths.push_back(std::filesystem::path(programs) / "Steinberg" / "VSTPlugins");
    }
#elif defined(__APPLE__)
    paths.emplace_back("/Library/Audio/Plug-Ins/VST");
    if (const std::string home = environment("HOME"); !home.empty())
        paths.push_back(std::filesystem::path(home) / "Library" / "Audio" / "Plug-Ins" / "VST");
#else
    if (const std::string home = environment("HOME"); !home.empty())
        paths.push_back(std::filesystem::path(home) / ".vst");
    paths.emplace_back("/usr/lib/vst");
    paths.emplace_back("/usr/local/lib/vst");
#endif
    return paths;
}

std::vector<std::filesystem::path> findVst2Files(const std::vector<std::filesystem::path>& directories)
{
#if defined(_WIN32)
    return plugin_files::findFiles(directories, ".dll");
#elif defined(__APPLE__)
    return plugin_files::findFiles(directories, ".vst");
#else
    return plugin_files::findFiles(directories, ".so");
#endif
}

} // namespace vloom
