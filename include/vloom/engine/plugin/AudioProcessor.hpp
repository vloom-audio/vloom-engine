// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vloom {

/// What a processor is (ADR 0006).
struct PluginInfo {
    enum class Kind { Effect, Instrument };
    std::string id; ///< unique and stable, e.g. "com.vendor.reverb" or "vloom.gain"
    std::string name;
    std::string vendor;
    std::string version;
    Kind kind = Kind::Effect;
    /// What it does, for a host's lists. The built-ins give one of
    /// "Dynamics", "EQ/Filter", "Delay/Reverb", "Modulation", "Distortion",
    /// "Utility" (effects) or "Synth", "Sampler", "Drums" (instruments).
    /// Empty: not known (hosted plugins; a host reads their features).
    std::string category{};
};

/// One parameter a processor exposes. Values are in the parameter's own
/// units, between minValue and maxValue.
struct ParameterInfo {
    std::uint32_t id = 0; ///< stable across versions and sessions
    std::string name;
    std::string unit; ///< e.g. "dB", "Hz", "%"; empty: none
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool stepped = false; ///< whole numbers only (a switch, a choice)
    /// What it belongs to, for a host that lays out its own controls, e.g.
    /// "Low Shelf"; parameters of one group follow each other. Empty: none.
    std::string group{};
    /// A stepped parameter that is a choice: the name of each whole value
    /// from minValue up (valueLabels[0] names minValue), e.g. "Peak", "RMS"
    /// or a note length "1/8 D". Empty: its values read as numbers (or as a
    /// switch when it goes 0 .. 1). Built-ins only, for now.
    std::vector<std::string> valueLabels{};
};

/// One place for a sample in a processor that plays samples (the built-in
/// sampler and drum sampler), as AudioProcessor::sampleSlot() reports it.
struct SampleSlotInfo {
    std::string name; ///< e.g. "Sample", "Pad 1"
    /// The file the slot plays, as given to loadSample() or a loaded state
    /// (UTF-8); empty: none. Kept when the file could not be read, so
    /// saving the project again does not lose it.
    std::string path;
    int numChannels = 0; ///< what is playing: 0 when nothing
    std::int64_t frames = 0;
    double sampleRate = 0.0;
    /// Why the file in `path` is not playing (missing, unreadable); empty
    /// when it plays or there is none.
    std::string error{};

    [[nodiscard]] bool loaded() const noexcept { return frames > 0; }
};

/// Where a sample slot reads `path` from: a WAV file the engine can decode
/// (e.g. a decoded copy of an MP3 in a host's cache). Returns the file to
/// read, or empty with the reason in `*error` (which may be null). Called on
/// the thread that loads, never the audio thread. Without one, `path` itself
/// is read, and must be a WAV file.
using SampleFileResolver = std::function<std::string(const std::string& path, std::string* error)>;

/// A parameter change at a frame of the block.
struct ParameterEvent {
    int frame = 0; ///< 0 .. frames - 1
    std::uint32_t id = 0;
    double value = 0.0;
};

/// A note starting or ending at a frame of the block (instruments).
struct NoteEvent {
    enum class Type : std::uint8_t { On, Off };
    int frame = 0;
    Type type = Type::On;
    std::int16_t key = 60; ///< MIDI key, 0 .. 127
    float velocity = 1.0f; ///< 0 .. 1
    /// The MIDI channel, 0 .. 15. With MPE each note sounding has a channel
    /// of its own, so that the controls sent on that channel bend and shape
    /// that note alone; without, everything is on channel 0.
    std::int16_t channel = 0;
};

/// What an instrument is played with besides notes: MIDI's continuous
/// messages, at a frame of the block.
struct ControlEvent {
    enum class Type : std::uint8_t { PitchBend, Controller, Pressure };
    int frame = 0;
    Type type = Type::Controller;
    std::int16_t number = 0; ///< Controller: which one, 0 .. 127 (1 = the mod wheel)
    float value = 0.0f; ///< PitchBend: -1 .. +1 (0 = centre); the others: 0 .. 1
    std::int16_t channel = 0; ///< the MIDI channel, 0 .. 15 (NoteEvent::channel)
};

/// What the host tells a processor about the song while it processes.
struct TransportInfo {
    bool playing = false;
    double tempo = 120.0; ///< beats per minute
    double positionTicks = 0.0; ///< at the block's first frame (kPpq per beat)
    /// The loop region in ticks; no loop when loopEndTicks <= loopStartTicks.
    double loopStartTicks = 0.0;
    double loopEndTicks = 0.0;
    /// The time signature: beatsPerBar notes of 1 / beatUnit. The engine
    /// counts in 4/4.
    int beatsPerBar = 4;
    int beatUnit = 4;
};

/// One block for AudioProcessor::process(). The audio is processed in place:
/// `channels` hold the input and receive the output (an instrument gets
/// silence in and adds its sound).
struct ProcessContext {
    float* const* channels = nullptr;
    int numChannels = 0; ///< 2 (stereo) for now
    int numFrames = 0; ///< at most the maxBlockFrames given to activate()
    std::span<const ParameterEvent> parameters; ///< in frame order
    std::span<const NoteEvent> notes; ///< in frame order
    TransportInfo transport;
    std::span<const ControlEvent> controls{}; ///< in frame order
    /// A second input, e.g. what a compressor listens to (its sidechain):
    /// two channels of numFrames, or null when the host routes none to it.
    const float* const* sidechain = nullptr;
    /// Room for the outputs a processor has after its main one, two
    /// channels each: [2k] and [2k + 1] are output k + 2. The processor
    /// writes all of them, silence where it has no such output.
    std::span<float* const> extraOutputs{};
    /// Where a processor that makes notes (an arpeggiator) puts them, in
    /// frame order; it sets *notesOutCount. Empty: the host wants none.
    std::span<NoteEvent> notesOut{};
    int* notesOutCount = nullptr;
};

/// The size of a plugin's editor: pixels on Windows and Linux, points on
/// macOS (as the platform's window system counts them).
struct EditorSize {
    int width = 0;
    int height = 0;
};

/// What a processor tells its host about its open editor (ADR 0009).
/// Called on the control thread, from inside the processor's calls or idle().
class EditorListener {
public:
    virtual ~EditorListener() = default;
    /// The editor wants another size: resize the window that holds it, then
    /// call AudioProcessor::setEditorSize().
    virtual void editorResizeRequested(EditorSize size) = 0;
    /// The plugin closed its editor itself: close the window that held it.
    virtual void editorClosed() { }
    /// A parameter was changed in the editor. The processor already applies
    /// it: this is for the host's own view (knobs, "project changed").
    virtual void parameterEdited(std::uint32_t id, double value)
    {
        (void)id;
        (void)value;
    }
};

/// What every hosted processor implements, whatever its format (CLAP, VST3,
/// built-in). See ADR 0006 for the threading contract:
///
/// - control thread: info(), parameters(), activate(), deactivate(),
///   latencyFrames(), saveState(), loadState(); never while it is processing
///   for activate() / deactivate() (the engine stops using it first);
/// - audio thread: process() and reset(), which must not lock, allocate,
///   free or wait.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    AudioProcessor() = default;
    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;

    [[nodiscard]] virtual PluginInfo info() const = 0;
    [[nodiscard]] virtual std::vector<ParameterInfo> parameters() const { return {}; }
    /// The value of parameter `id` as last set (by the host, a loaded state
    /// or the processor's defaults), for a host that draws its own controls.
    /// nullopt: unknown id, or the processor does not report values (plugins
    /// show theirs in their own editor). Control thread.
    [[nodiscard]] virtual std::optional<double> parameterValue(std::uint32_t id) const
    {
        (void)id;
        return std::nullopt;
    }

    /// Stereo outputs it has after the main one (a drum machine's pads, a
    /// sampler's groups), for a host that routes them apart. As of the
    /// last activate(); 0 before. Control thread.
    [[nodiscard]] virtual int extraOutputCount() const { return 0; }
    /// True if it has a second input (a sidechain). As extraOutputCount().
    [[nodiscard]] virtual bool hasSidechain() const { return false; }
    /// True if it puts out notes (ProcessContext::notesOut).
    [[nodiscard]] virtual bool makesNotes() const { return false; }

    /// How much it turns the sound down now, in dB (>= 0, the most during
    /// its last block), for a meter: the built-in compressor, gate, limiter
    /// and de-esser. nullopt: it reports none. Any thread.
    [[nodiscard]] virtual std::optional<float> gainReductionDb() const { return std::nullopt; }

    /// Sample slots (the built-in sampler: 1, the drum sampler: one per
    /// pad). Not real-time: from the control thread or a worker thread, one
    /// call at a time per processor (it serializes them with activate()),
    /// never the audio thread; process() may run meanwhile, and a new sample
    /// is picked up with the next block. Replaced samples are released by
    /// idle(). saveState() keeps each slot's path, and loadState() loads the
    /// files again; a file it cannot read is reported in sampleSlot().error,
    /// it does not fail the state.
    [[nodiscard]] virtual int sampleSlotCount() const { return 0; }
    /// What slot `slot` holds; empty for a bad slot.
    [[nodiscard]] virtual SampleSlotInfo sampleSlot(int slot) const
    {
        (void)slot;
        return {};
    }
    /// Reads the audio file `path` (through the resolver, if one is set)
    /// and plays it in `slot` from the next block. False with the reason in
    /// `*error` (may be null) if the slot does not exist or the file cannot
    /// be read; the slot then keeps what it had.
    virtual bool loadSample(int slot, const std::string& path, std::string* error = nullptr)
    {
        (void)slot;
        (void)path;
        if (error != nullptr)
            *error = "this processor has no sample slots";
        return false;
    }
    /// Empties `slot`; false for a bad slot.
    virtual bool clearSample(int slot)
    {
        (void)slot;
        return false;
    }
    /// Sets where sample files are read from (see SampleFileResolver), for
    /// loadSample() and loadState(). Set it before loading a state.
    virtual void setSampleFileResolver(SampleFileResolver resolver) { (void)resolver; }

    /// The names of the programs (presets) it lists for a host to choose
    /// from; empty if it lists none. Control thread.
    [[nodiscard]] virtual std::vector<std::string> programs() const { return {}; }
    /// The program it is on, as an index into programs(); -1: unknown.
    [[nodiscard]] virtual int currentProgram() const { return -1; }
    /// Switches to program `index`; false if it cannot.
    virtual bool setProgram(int index)
    {
        (void)index;
        return false;
    }

    /// True once the processor has stopped working for good, e.g. a plugin
    /// that ran in a process of its own which has ended. It then passes
    /// audio through and saveState() gives nothing: a host keeps the state
    /// it last had and tells the user. Control thread.
    [[nodiscard]] virtual bool hasFailed() const { return false; }

    /// Gets ready to process at `sampleRate`, in blocks of at most
    /// `maxBlockFrames`. False if it cannot (the host then leaves it out).
    virtual bool activate(double sampleRate, int maxBlockFrames) = 0;
    virtual void deactivate() { }
    /// Frames the processor delays its output by.
    [[nodiscard]] virtual int latencyFrames() const { return 0; }

    /// Its whole state (parameters and anything else), to save in a project.
    [[nodiscard]] virtual std::vector<std::uint8_t> saveState() const { return {}; }
    /// Restores a saveState() result; false if it cannot read it.
    virtual bool loadState(std::span<const std::uint8_t> state)
    {
        (void)state;
        return false;
    }

    /// Control thread, often (a UI timer; ~60 Hz while an editor is open):
    /// lets the processor do work it asked the host to run on the main
    /// thread, run its editor's timers and report edits made in it.
    virtual void idle() { }

    /// Editors (ADR 0009), control thread only.
    ///
    /// `parent` is the native window the editor goes into: an HWND on
    /// Windows, an NSView* on macOS, an X11 Window id (cast to a pointer) on
    /// Linux. The listener must outlive the open editor. Opening an open
    /// editor closes it first. False if the processor has no editor or it
    /// could not be opened.
    [[nodiscard]] virtual bool hasEditor() const { return false; }
    virtual bool openEditor(void* parent, EditorListener* listener)
    {
        (void)parent;
        (void)listener;
        return false;
    }
    virtual void closeEditor() { }
    [[nodiscard]] virtual bool isEditorOpen() const { return false; }
    /// The open editor's size; zero when closed.
    [[nodiscard]] virtual EditorSize editorSize() const { return {}; }
    [[nodiscard]] virtual bool canResizeEditor() const { return false; }
    /// Asks the open editor to take `wanted`; returns the size it took (it
    /// may adjust it, or keep its own if it cannot resize).
    virtual EditorSize setEditorSize(EditorSize wanted)
    {
        (void)wanted;
        return editorSize();
    }
    /// The host is setting parameter `id` to `value` (PluginInstance::
    /// setParameter, before the audio thread gets it), so an open editor can
    /// follow. True: the processor has taken the value itself and gives it
    /// to its audio thread with the next block, so the host must not send it
    /// as a ParameterEvent too. Control thread.
    virtual bool parameterChangedByHost(std::uint32_t id, double value)
    {
        (void)id;
        (void)value;
        return false;
    }

    /// Processes one block (audio thread).
    virtual void process(const ProcessContext& context) noexcept = 0;
    /// Forgets what it was playing or holding (tails, notes), e.g. on a jump
    /// in the song (audio thread).
    virtual void reset() noexcept { }
};

} // namespace vloom
