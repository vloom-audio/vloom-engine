// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>
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
};

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
};

/// What the host tells a processor about the song while it processes.
struct TransportInfo {
    bool playing = false;
    double tempo = 120.0; ///< beats per minute
    double positionTicks = 0.0; ///< at the block's first frame (kPpq per beat)
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
