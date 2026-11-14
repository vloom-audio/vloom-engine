# 0006. Plugin host interface

- Status: accepted
- Date: 2026-11-14

## Context

M4 brings third-party plugins into the engine, CLAP first, VST3 in M5.
Every format has its own API, threading rules and lifecycle. The rest of the
engine (the mixer, later the channel rack) should not care which format a
processor comes from, and built-in effects should plug in the same way.

The real-time rules hold for plugins as for everything else on the audio
thread (docs/coding-guidelines.md): no locks, no allocation, no freeing. The
engine's state hand-over (ADR 0005) must also cover plugin instances: an
instance removed from the mixer must not be destroyed while the audio thread
may still be processing it, and never on the audio thread.

## Decision

1. **One interface, `plugin/AudioProcessor`.** What every hosted processor
   implements, whatever its format: `info()` (id, name, vendor, kind),
   `parameters()`, `activate(sampleRate, maxBlockFrames)` /
   `deactivate()`, `latencyFrames()`, `saveState()` / `loadState()` on the
   control thread; `process(ProcessContext&)` and `reset()` on the audio
   thread. Format adapters (the CLAP host, then VST3) and built-in
   processors implement it. Processing is in place on a stereo bus, and the
   context carries the block's parameter changes and note events, each at
   its frame, and the transport (tempo, position, playing).
2. **The engine owns a wrapper, `plugin/PluginInstance`.** It holds the
   processor and adds what the host needs: activation state, bypass, and an
   SPSC queue of parameter changes from the control thread
   (`setParameter()`), drained at the start of each block into the context.
   A processor that is not active, or is bypassed, passes audio through
   untouched, so a failed activation never silences or corrupts a bus.
3. **Plugins live in the mixer snapshot.** `InsertState::effects` is the
   insert's effect chain, `std::shared_ptr<PluginInstance>` in order. As with
   channel voices (ADR 0005), the instance is created on the control thread
   and shared by every snapshot that contains it; the audio thread only uses
   raw pointers. An instance is destroyed when the last snapshot holding it
   is collected on the control thread, which happens only after the audio
   thread released that snapshot. An insert with effects is processed every
   block, even when nothing plays into it, so tails (reverb, delay) ring out.
4. **Activation is the control thread's job.** The host activates an
   instance before publishing a state that holds it. `Engine::prepare()` (a
   new sample rate or block size, called while audio is stopped) activates
   again every instance in the current mixer state.
5. **Latency is reported, not compensated yet.** Plugin delay compensation
   comes with M5; until then `latencyFrames()` is informational.

6. **Instruments play channels.** A channel with `Instrument::Plugin` holds
   a `PluginInstance` (`ChannelState::plugin`). Its steps and piano-roll
   notes become `NoteEvent`s at the frame they fire on (`PluginNotes`, in
   the channel's voices, tracks the held keys like the built-in voices do);
   the plugin processes the whole block once, in `StepSequencer::mix()`,
   every block while set, then the channel's volume, pan and insert apply as
   for any channel. Plugins are told the transport as at the block's start.
7. **CLAP is the first format** (`plugin/ClapHost`). `ClapLibrary` loads a
   .clap once per process and lists its plugins; `create()` wraps one in an
   `AudioProcessor` that keeps the library loaded. The adapter sizes a
   buffer for every audio port at activation, copies our stereo into the
   main input and back from the main output (an instrument without inputs
   adds to the channel), passes parameters and notes as CLAP events from a
   fixed event array, and answers no host extensions yet. The control thread
   is CLAP's main thread; `AudioProcessor::idle()` runs what a plugin asked
   for with `request_callback`. `start_processing()` runs on the audio
   thread before the first block; `stop_processing()` runs at deactivation,
   which the host only does once the audio thread stopped using the plugin.
   `clapSearchPaths()` and `findClapFiles()` find installed plugins (the
   CLAP spec's directories, after `CLAP_PATH`).
8. **VST 3 is the second format** (`plugin/Vst3Host`, M5).
   - **What it is built on.** Only Steinberg's interface headers are used
     (`vst3_pluginterfaces`, MIT since VST 3.8). The engine implements the
     host side itself rather than taking the SDK's hosting helpers.
   - **Loading.** `Vst3Library` loads a .vst3 once per process. That is a
     bundle directory, using the binary for this OS and architecture, or a
     single file. It runs the module's entry function (`InitDll`,
     `bundleEntry` or `ModuleEntry`) and lists the classes of the "Audio
     Module Class" category.
   - **Plugin ids.** A plugin's id is its class id as 32 hex digits. Its
     kind comes from the sub-categories: "Instrument" makes it an
     instrument.
   - **Creating a plugin.** `create()` makes the component and its edit
     controller. That is the same object, or a separate class that is
     initialised and connected both ways through `IConnectionPoint`. The
     host then gives the controller the component's state.
   - **Buses.** At activation the adapter asks for 32-bit stereo main
     buses, sizes a buffer for every bus, and activates the main buses and
     the event input.
   - **Processing.** Parameters arrive as normalized values (0..1) in fixed
     parameter queues, notes in a fixed event list, and the transport in a
     `ProcessContext`. An instrument without audio inputs adds to the
     channel; an effect replaces the audio.
   - **Reset.** VST 3 has no reset, so `reset()` ends every held note in the
     next block.
   - **State.** `saveState()` stores the component's and the controller's
     state together.
   - **Finding plugins.** `vst3SearchPaths()` and `findVst3Files()` look in
     Steinberg's directories, after `VST3_PATH`.

## Alternatives considered

- **Format-specific code in the mixer**: every format would leak into the
  mixer and the scheduler; built-in effects would be a third path.
- **Commands for parameter changes through the engine's command queue**: the
  queue carries engine-wide controls; per-instance queues keep one plugin's
  automation from crowding out transport commands and need no lookup.
- **Out-of-process hosting from the start**: protects the engine from a
  crashing plugin, but costs a block of latency and a lot of machinery. The
  app already scans plugins in a child process; out-of-process processing
  can come later behind the same `AudioProcessor` interface.

## Consequences

- Built-in effects and format adapters are interchangeable in the mixer and
  in tests.
- Every processor must keep the audio-thread rules: `process()` and `reset()`
  are `noexcept`, allocate nothing and lock nothing. Adapters must enforce
  this around third-party code as far as possible (pre-allocated buffers,
  event lists).
- A plugin that blocks or crashes in `process()` still takes the audio
  thread with it; out-of-process hosting is the remedy if that proves common.
- Loading a .clap or a .vst3 runs third-party code with the host's rights. The engine
  loads only the paths the host gives it; the app decides what to trust
  (installed plugin folders, the user's choice).
- Not yet: CLAP host extensions (params rescan, latency changes, GUI,
  note ports, thread-check), `request_restart` (recorded, not acted on),
  plugin output events, sidechain and extra ports (given silence), the
  master pitch on plugin instruments. These follow as plugins need them.
  The VST 3 adapter has no editor (`IPlugView`) yet. It ignores
  `restartComponent()` except to note latency changes, and it does not
  read the plugin's output parameter changes or events.
