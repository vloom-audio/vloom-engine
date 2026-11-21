# Changelog

All notable changes to vloom-engine are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- ASIO drivers on Windows (ADR 0021), through the Steinberg ASIO SDK 2.3.4
  used under its GPLv3 option. **Licence:** that option is GPL version 3
  only, so a Windows binary built with ASIO (`VLOOM_ENGINE_ASIO`, default
  ON on Windows) is GPL-3.0-only; the source stays GPL-3.0-or-later, and
  `-DVLOOM_ENGINE_ASIO=OFF` builds without it. The SDK is downloaded at
  configure time (pinned by SHA-256) or taken from
  `VLOOM_ENGINE_ASIO_SDK_DIR`; no SDK file is in the repository.
  - `AudioApi { Default, Asio }`: `AudioDeviceConfig::api`,
    `AudioInputConfig::api`, `listOutputDevices(AudioApi)`,
    `listInputDevices(AudioApi)` (the installed drivers),
    `AudioDevice::isApiAvailable()`, `AudioDevice::api()`,
    `AudioInput::api()`. The defaults keep every existing path unchanged.
  - Full duplex on one clock: `AudioDeviceConfig::inputChannels` opens
    driver inputs with the output, and an `AudioInput` with `AudioApi::Asio`
    taps them; its callback runs on the driver's thread just before the
    output's. One ASIO driver per process.
  - The requested rate or a clear error (driver name and ASIO error); the
    requested buffer size when the driver allows it, else its preferred
    one (`AudioDevice::bufferFrames()`).
  - `AudioDevice::inputLatencyFrames()` and `outputLatencyFrames()` from
    `ASIOGetLatencies`, for `Engine::setAudioRecordLatency()`.
  - `AudioDevice::showControlPanel()` (the driver's panel; false for other
    APIs; a modal panel blocks stop and close until it is closed),
    `AudioDevice::resetRequested()` (the driver asked for a reset, a
    buffer-size or a rate change: close and open again, not for what it
    says while opening or starting) and `AudioDevice::reopenBufferFrames()`
    (the size to reopen with after a buffer-size change). A latency change
    is no reset: the latency getters ask the driver again.
  - An ASIO input opened against an earlier stream of the output fails to
    start and must be opened again.
  - Sample formats Int16, Int24 (packed), Int32, Float32, Float64 and
    Int32 with 16/18/20/24-bit alignment, little-endian; output clamped to
    [-1, 1]. Big-endian and DSD drivers fail to open with a message.
  - Elsewhere the API is there but ASIO lists nothing and fails to open
    ("ASIO is not available on this platform").
- WASAPI exclusive mode on Windows (ADR 0020): `AudioDeviceConfig::exclusive`
  opens the output without the Windows mixer, its rate conversion or its
  limiter, and `AudioDevice::isExclusive()` reports whether the open stream
  got it. The device must take the requested rate and channel count; the
  first format it accepts of float32, int32, int24 in 32 bits, packed int24
  and int16 is used, and samples beyond [-1, 1] hard-clip. A busy device,
  a device that forbids exclusive use or one that takes none of these
  formats fails `open()` with a message; there is no fallback to shared
  mode. Other platforms ignore the flag. Shared mode is unchanged.
- The tuner (`vloom.tuner`) and the spectrum analyzer (`vloom.analyzer`)
  built-ins, under the new category "Analysis" (vloom-dsp ADR 0005). The
  analyzer passes audio through; a host draws its spectrum from the
  instance's scope.
- vloom-dsp's readouts (`HasReadouts`) reach `AudioProcessor::readouts()`
  (ADR 0019): the tuner's frequency_hz, note, cents, has_pitch, confidence
  and level_db, and the analyzer's peak_db. The dynamics' gain reduction
  and the gate's open state come from vloom-dsp too, under the same ids
  as before ("gainReduction", "gateOpen"); the gate now lists "gateOpen"
  first, so look readouts up by id.
- Data for editors that a host draws itself (ADR 0019):
  - Per-instance level meters for every plugin, CLAP, VST 3, VST 2 or
    built-in: `PluginInstance::setMetersEnabled()` and `readLevels()`. Each
    read gives the input peak (before the processor) and the output peak
    (after it) per channel since the last read, lock-free. Off by default
    and free while off.
  - A stereo scope: `PluginInstance::readScopeStereo(left, right)`, which
    keeps left and right apart for a vectorscope or correlation meter.
    `readScope()` still returns the mono average.
  - Generic readouts: `AudioProcessor::readouts()` (`ReadoutInfo`: id,
    name, unit, toggle) and `readoutValue(index)`, readable from any
    thread. The built-in compressor, gate, limiter and de-esser report
    "gainReduction" (dB), and the gate also reports "gateOpen".
    `gainReductionDb()` stays.
  - A sample slot's audio for a waveform: `AudioProcessor::sampleAudio(slot)`
    (`std::shared_ptr<const SampleAudio>`, sharing the processor's own
    copy and safe to hold across a reload). `sampleSlotsVersion()` is a
    lock-free counter that changes with every load, clear or loaded state.
- All 20 built-in processors of vloom-dsp (ADR 0008, vloom-dsp pinned to
  b469c4f), which completes M5's built-in effects: compressor, limiter,
  gate, de-esser, transient shaper, EQ, filter, delay, reverb, chorus,
  flanger, phaser, tremolo, saturator, bitcrusher, gain and utility, and the
  synth, sampler and drum sampler instruments.
  - `PluginInfo::category`: what a built-in is for a host's lists
    ("Dynamics", "EQ/Filter", "Delay/Reverb", "Modulation", "Distortion",
    "Utility", "Synth", "Sampler", "Drums").
  - `ParameterInfo::valueLabels`: the names of a stepped choice's values
    (modes, curves, filter types, note lengths such as "1/8 D", key names).
  - The compressor and gate have a sidechain (`hasSidechain()`): a mixer
    sidechain send keys them, delay-compensated like a plugin's.
  - The delay, chorus, flanger, phaser and tremolo follow the transport's
    tempo.
  - The limiter's and saturator's latency follows their Lookahead, True
    Peak and Oversampling: `Engine::updatePluginLatency()` picks it up.
  - `AudioProcessor::gainReductionDb()` for a meter (compressor, gate,
    limiter, de-esser).
  - Sample slots for the sampler (1) and the drum sampler (16, one per pad):
    `AudioProcessor::sampleSlotCount()`, `sampleSlot()` (`SampleSlotInfo`),
    `loadSample()`, `clearSample()` and `setSampleFileResolver()` (for files
    a host decodes itself). The state keeps each slot's path and loads the
    files again; a missing file is reported in `SampleSlotInfo::error`, not a
    failed state. `idle()` releases replaced samples.
- Count-in and punch recording (ADR 0017):
  - `Engine::setCountIn()` (0 .. 4 bars): playing from stop with a
    recording armed first counts the bars with the metronome, the song
    standing still, then starts it on the exact sample; nothing is recorded
    meanwhile. `setCountInClick()` clicks it with the metronome off;
    `isCountingIn()`, `countInBarsLeft()` and `countInBeatsLeft()` tell the
    UI. `setBeatsPerBar()` sets the time signature's beats (metronome
    accent, count-in bars, plugins' transport).
  - `Engine::setPunch()`: audio and MIDI are recorded only where the player
    heard the punch range, in every loop pass (audio to the sample, mid-block
    too; live MIDI to the block it arrived in, as its position is),
    the latency taken off at both edges. Audio takes get a `Punch` mark per
    stretch (`AudioRecordMark::Kind::Punch`, read by `AudioTake`);
    `MidiTake::setPunch()` leaves out notes begun outside the range and ends
    notes held over the punch-out there.
- Bouncing to audio (ADR 0018): `bouncePattern()` renders one pass of a
  pattern (or the channel rack's own, `kChannelRackPattern`) and
  `bounceRange()` part of the song, into memory or a WAV file, with a choice
  of channels and of mix (`BounceMix::Dry`, `Inserts`, `Master`), tail
  included; `patternLengthTicks()`. Same offline path as `exportSong()`, so
  it sounds exactly like the pattern played.
- Audio input, monitoring and recording it (ADR 0016), the second half of M6:
  - `AudioInput` captures from a microphone or an audio interface: WASAPI
    on Windows, CoreAudio on macOS, ALSA on Linux.
  - `Engine::pushAudioInput()` takes it in through a lock-free ring;
    `setInputMonitor()` plays it into an insert; `inputDropouts()` counts
    blocks it was late for.
  - `Engine::setAudioRecording()` records it while the song plays, with
    marks for where takes start and the song jumps;
    `setAudioRecordLatency()` gives the device's round trip.
    `AudioTake` writes it to a WAV file as it comes and says which stretch
    of the file plays where in the song (`AudioTakeSegment`), each loop
    pass and each take apart.
  - `SpscRing`, a lock-free ring of floats.
- MIDI input and recording (ADR 0015), the first half of M6:
  - `MidiInput` reads a MIDI port: WinMM on Windows, CoreMIDI on macOS,
    the ALSA sequencer on Linux. `MidiMessage` and `decodeMidi()` read
    what it sends.
  - `Engine::playMidi()` plays a message live on any channel, from any
    thread: the synth, the sampler (its sample at the key) and instrument
    plugins (with the MIDI channel, through the note effects). A note is
    held until its note-off, whatever the song does meanwhile; all notes
    off (controller 123 or 120), `Engine::releaseLiveNotes()` and a
    `MidiInput` being stopped or closed end it.
  - `Engine::setRecording()` records what is played while the song plays,
    at the position the player heard it (`setRecordLatency()` and the
    plugins' latency taken off; loop passes counted).
    `takeRecordedMidi()` hands it over; `MidiTake`, `quantize()`,
    `patternNotes()` and `patternControls()` turn it into a pattern's notes
    and controls.
  - `Synth` and `Sampler` gain `releaseExactly()` and `kLiveFirst`, and
    `stopAll(keepLive)`.
- Surround plugins in the stereo mixer (ADR 0014): a VST 3 or CLAP plugin
  whose main bus is wider than stereo runs in its own layout. The host's
  left and right go to its left and right speakers, and its speakers are
  folded down to stereo by the usual shares (the centre to both at -3 dB,
  the bass channel left out). Before, only its first two channels were
  used. The mixer itself stays stereo.
- MPE (ADR 0013): `NoteEvent`, `ControlEvent` and `ControlPoint` carry a
  MIDI channel, and `PluginInstance::playNote()`, `stopNote()` and
  `sendControl()` take one. CLAP, VST 3 and VST 2 plugins (bridged ones
  too) get each note and control on its channel, and the notes they put
  out come back with theirs, so an MPE instrument can bend and shape each
  note on its own. Channel 0 by default: nothing changes for code that
  knows no channels.
- A channel's instrument can play another channel's notes as well as its
  own: `ChannelState::notesFrom`, taken after the source's note effects.
- Pitch bend, controllers and pressure can be written into the song:
  `ChannelState::controls` (pattern mode) and `SongPattern::controls`
  (song mode) are `ControlPoint`s that play at their ticks, on their exact
  frames, like notes. `PluginInstance::Io::controls` carries them.
- Plugins get more than their main sound (ADR 0012), for CLAP, VST 3 and
  VST 2 alike:
  - **Sidechains.** `Send::sidechain` sends an insert's sound to another
    insert's sidechain, where its effects with a second input hear it (a
    compressor ducking to a kick). `AudioProcessor::hasSidechain()`.
  - **Extra outputs.** `ChannelState::pluginOutputs` names an insert for
    each output an instrument has after its main one (a drum machine's
    pads). `AudioProcessor::extraOutputCount()`.
  - **Note effects.** `ChannelState::noteEffect` is a plugin that makes
    notes from the channel's (an arpeggiator); the instrument plays what it
    puts out. `AudioProcessor::makesNotes()`.
  - **Pitch bend, controllers and pressure.**
    `PluginInstance::sendControl()`, e.g. for a MIDI keyboard's wheels.
  - **Programs.** `AudioProcessor::programs()`, `currentProgram()` and
    `setProgram()` (VST 3 and VST 2).

  `ProcessContext` carries all of it; processors that use none need no
  change. `PluginInstance::process()` has an overload with an `Io`.

  Also: `ChannelState::noteEffects` for several note effects in a row;
  notes played by hand on an instrument go through its note effects; an
  effect behind one with latency hears the sidechain as late as its sound;
  `droppedRoutes()` tells which routes the mixer leaves out; a bridged
  VST 2 plugin gets a sidechain, eight more output pairs, its notes and its
  programs across the bridge, and a bridge that ended is started again
  with the plugin's last saved state.
- VST 2 plugins are hosted (ADR 0010): `plugin/Vst2Host` has `Vst2Library`
  (`open`, `plugins`, `create`), `vst2SearchPaths()` and `findVst2Files()`.
  Effects and instruments, 32-bit audio, notes as MIDI, parameters, state,
  latency, and editors on Windows and macOS. The interface is described in
  the engine itself; no Steinberg SDK is used. Shell files and editors on
  Linux are not hosted.
- Windows: 32-bit VST 2 plugins run in a 64-bit host (ADR 0011). The new
  `vloom-plugin-bridge` tool loads the plugin in a process of its own and
  shares audio and requests with the engine through memory. Built for 32
  bits it is `vloom-plugin-bridge32.exe`; `Vst2Library::open()` uses it by
  itself for a 32-bit file when it lies next to the application (or where
  `VLOOM_PLUGIN_BRIDGE32` says). `Vst2Library::openBridged()` runs any
  VST 2 plugin that way, so that a crash of the plugin ends the bridge and
  not the host.
- `AudioProcessor::hasFailed()`: true once a processor has stopped for good
  (a bridged plugin whose process ended). Its `saveState()` then gives
  nothing, so a host can keep the state it last had.
- VST 2 plugins report `parameterValue()`, for a host that draws its own
  controls.
- `TransportInfo` carries the loop region (`loopStartTicks`, `loopEndTicks`)
  and the time signature (`beatsPerBar`, `beatUnit`; the engine counts in
  4/4). CLAP, VST 3 and VST 2 plugins are told both, and where the bar
  starts, so tempo-synced effects can follow the bar.
- CLAP: a plugin whose note port takes MIDI only gets its notes as MIDI
  events. Before, it got CLAP note events and stayed silent.
- CLAP: the host offers the `thread-check`, `log`, `latency`, `state`,
  `audio-ports` and `note-ports` extensions, for plugins that do not start
  without them.
- Built-in instrument `vloom.synth`: a wavetable synthesizer from vloom-dsp
  (two wavetable oscillators with unison, sub, noise, a filter, three
  envelopes, four LFOs). Built-in instruments get the block's notes at their frames.
- Built-in effect `vloom.eq`: a parametric equalizer from vloom-dsp (low and
  high cut, low and high shelf, two bells).
- `AudioProcessor::parameterValue()`: the value of a parameter as last set,
  for a host that draws its own controls. The built-ins report theirs.
- `ParameterInfo::group`: what a parameter belongs to ("Low Shelf").
- `PluginInstance::playNote()` and `stopNote()`: notes played by hand (an
  editor's keyboard), delivered to the plugin with its next block.
- `PluginInstance::setScopeEnabled()` and `readScope()`: a copy of the sound
  a hosted plugin puts out, for an editor that draws it (a spectrum behind an
  equalizer's curve). Off by default; nothing is copied until it is on.

### Changed
- **Breaking:** `AudioProcessor::parameterChangedByHost()` returns `bool`:
  true when the processor has taken the value itself, and
  `PluginInstance::setParameter()` then does not queue it. The built-ins do,
  so their values are kept (state, `parameterValue()`) while no audio runs
  and a full queue can no longer deliver an older value after a newer one.
- **Breaking:** `plugin/GainProcessor` is removed. Use
  `createBuiltinProcessor("vloom.gain")` instead; the id and parameter 0
  (dB) stay the same. A change now glides over 5 ms rather than over the
  block it arrives in.

### Security
- Supply chain: GitHub Actions are pinned to commit SHAs (checkout v7.0.1, cache
  v6.1.0; Dependabot keeps them current), the CPM.cmake download is checked against a
  pinned SHA-256 (a cached copy that does not match is fetched again), and Catch2 is
  pinned to the commit of its v3.7.1 tag.

### Fixed
- VST 3: the host gives the module's factory its host context
  (`IPluginFactory3::setHostContext`) before it reads the classes. A plugin
  shell (WaveShell) lists its edit controllers only after that, so its
  plugins loaded without parameters or an editor, or not at all.
- VST 3: `saveState()` returned nothing for a plugin whose edit controller
  refuses to write a state of its own (Sylenth1, Nexus, Kickstart 2,
  Addictive Drums 2 and others), so a saved project lost everything set in
  those plugins. The component's state is saved without it now, and
  `loadState()` gives each part only to where it came from.
- VST 3: a plugin with more than 16 audio buses in one direction (Battery,
  Kontakt and other multi-output instruments) could not be activated. The
  limit is 128 now.
- CLAP: the same limit of 16 audio ports in one direction is 128 now.
- Windows: a plugin's own folder is searched first for the DLLs it brings
  along. Before, a plugin with a DLL beside it could fail to load.
- Song mode: an audio clip moved, cut, muted, added or removed while the song
  plays kept sounding as it was until playback was restarted. The channels
  whose clips changed now fade out and their clips play from where they are
  now. A clip taken out of a playing song fades at once.
- Windows: loading a file that is not a plugin (or a plugin whose own DLLs
  are missing) showed a system "Bad Image" message box and waited for the
  user. The loader now asks Windows to return the error, so
  `ClapLibrary::open()` and `Vst3Library::open()` just fail with a message.
  Load errors also name files outside the system code page without
  throwing.

### Fixed (engine audit)
- A tempo change while playing could fire a beat, step or note start twice,
  or skip it: the scheduler now carries each stream's next event over between
  blocks instead of working it out afresh from the position.
- Asking for the same preview sample again and again (17 times) left old
  preview states undrained, after which previews could no longer be changed
  or stopped. A preview asked for in the same block as Stop now plays.
- macOS: after the output device was lost, `stop()` left the audio unit
  running and `start()` reported success while playing silence; `start()`
  now fails until the device is reopened. Property listeners are block-based
  on a private queue that is drained on removal, so no notification can run
  into a destroyed backend or watcher.
- Voice stealing cut a voice outright when more notes than fade slots
  started at once; the quietest fading voice is now replaced, and there are
  8 fade slots instead of 4 (Sampler and Synth).
- Integer overflow from out-of-range clip positions and note lengths: clips
  reaching past `StepSequencer::kMaxTicks` are ignored, notes are held at
  most that long.
- `Engine::prepare()` clamps the sample rate to 8–768 kHz (non-finite:
  48 kHz); the synth's to 1–768 kHz, keeping its filter range valid.
- `renderInterleaved()` / `renderToWav()` render nothing for a negative or
  non-finite length; `renderToWav()` refuses a render too long for WAV
  before rendering and streams it block by block. The WAV header's byte rate
  is checked for overflow.
- `loadSample()` no longer throws (invalid UTF-8 paths, out of memory), and
  refuses to grow a file more than 16 times (or past 64 MB) by resampling a
  very low-rate file up to the engine's rate.
- An exception from an `AudioDevice` callback silences that block instead of
  ending the program.
- `Engine::prepare()` no longer throws away commands queued while the device
  was gone (a Stop or Locate pressed meanwhile now counts), and silences every
  voice: after a sample-rate change, notes no longer go on reading samples
  made for the old rate (out of tune). `sampleRate()` and `beatsPerBar()` are
  atomic, as "any thread" getters must be.
- Playing from less than a frame past an audio clip's start no longer starts
  it twice (+6 dB).
- macOS: the device "alive" listener shares only the flags it sets, not the
  backend, and `start()` no longer reports running on a device lost while it
  started.
- Voice stealing weighs a fading voice's gain (velocity), not only how far
  its fade has gone.
- `WavFileWriter` uses a temporary name of its own (no clash between two
  writers or with a user's file) and checks up front that an existing target
  can be replaced. `renderInterleaved()` caps block size, channels and the
  buffer it holds instead of throwing.
- `toBarBeatTick()` and `Metronome::prepare()` handle NaN and infinite input
  without undefined behaviour.
- `vloom-cli` validates its numbers (`--rate`, `--seconds`, `--bpm`,
  `--device`, `--buffer`): a rate outside 8–768 kHz used to load samples for
  one rate and play them at another.

### Security / CI
- CI's token is read-only (`permissions: contents: read`) and checkouts do
  not keep it (`persist-credentials: false`).
- The `asan` preset stops on undefined behaviour (`-fno-sanitize-recover=all`)
  instead of printing it and passing; `VLOOM_SANITIZER` with MSVC warns that
  it is ignored.
- The test allocation counter also counts aligned `operator new`, so
  `alignas` types cannot allocate on the audio thread unnoticed.

### Changed
- Song-mode clips sit on any tick, not only on steps (a playlist can snap
  them to 1/32 notes or finer). Breaking: `SongPatternClip` and
  `SongAudioClip` take `startTick`, `lengthTicks` and `offsetTicks` (kPpq per
  quarter note) instead of `startStep`, `lengthSteps` and `offsetSteps`;
  multiply old values by `ticksPerStep`. A pattern clip's steps stay on its
  pattern's grid: they play at `startTick - offsetTicks + n * ticksPerStep`.
  Song mode's steps and audio clip starts are scheduled with the note starts
  (`StepSequencer::nextNoteStart()` / `startNotes(tick, framesPerTick)`), so
  `ticksPerStep()` is 0 in song mode and `triggerStep(step)` plays pattern
  mode only. After a locate, starts less than a frame before the position
  fire on the first sample, as steps already did.
- `resample()` (used by `loadSample()`) works out each output frame's filter
  taps once for all channels instead of once per channel: about 30% faster on
  stereo files (a 4-minute song from 44.1 to 48 kHz: 4.6 s to 3.2 s in a
  release build), with the same output.
- `Engine::setSequencerState()` and `StepSequencer::publish()` take a
  `std::unique_ptr<SequencerState>`: notes are sorted by start on the control
  thread so the audio thread finds the next one by binary search (notes may
  still be given in any order).
- WAV files are written to `<path>.part` and renamed into place when complete
  (`WavFileWriter`), so a failed write never leaves a broken file.

### Added
- WAV loop tempo: `parseWavLoopInfo()` / `readWavLoopInfo(path)` read the
  ACID chunk that sample libraries and DAWs write into loops. It gives the
  tempo, the length in beats, and whether the file is a one-shot.
  - `readWavLoopInfo()` walks only the chunk headers and never reads the
    audio.
  - Every value is bounds-checked.
- Latency that changes while a plugin runs is compensated (ADR 0007):
  - `Engine::updatePluginLatency()` reads every insert effect's latency
    again. If one changed, it publishes the mixer state again, lined up for
    the new latency. Call it from the UI timer.
  - `PluginInstance::updateLatency()` does the same for one instance.
  - Before this, a track whose effect changed latency, such as FabFilter
    Pro-L 2 with oversampling switched on (3115 to 3142 frames), played out
    of time with the others until the effect was loaded again.
- Master effects (ADR 0007): `MixerState::masterEffects` is an effect
  chain on the master.
  - It processes everything the master gets, before the master gain and
    pan, every block, so tails ring out.
  - Its latency adds to `Engine::latencyFrames()`, and the metronome is
    delayed as much.
  - The metronome and the preview bypass it.
  - `Engine::prepare()` re-activates its effects with the inserts'.
- Plugin editors (ADR 0009):
  - `AudioProcessor::openEditor(parent, listener)` places a plugin's editor
    in a native window: an HWND, an NSView* or an X11 Window id. The API
    also has `hasEditor`, `closeEditor`, `isEditorOpen`, `editorSize`,
    `canResizeEditor` and `setEditorSize`.
  - `EditorListener` tells the host when the editor wants a new size, when
    the plugin closed it, and when a parameter was edited in it.
  - The CLAP host offers `gui`, `timer-support`, `posix-fd-support` and
    `params` (flush) to plugins. Parameter values the plugin reports reach
    the listener from `idle()`.
  - The VST 3 host opens `IPlugView` editors. `performEdit` reaches the
    processor in the next block and the listener at once. On Linux the
    frame offers an `IRunLoop`.
  - `PluginInstance::setParameter()` tells an open editor through
    `AudioProcessor::parameterChangedByHost()`.
- Built-in processors from vloom-dsp (M5, ADR 0008):
  - The engine pulls in vloom-dsp (MIT, our own DSP library) with CPM,
    pinned to a commit, and links it privately.
  - `plugin/BuiltinProcessors`: `builtinProcessors()` lists the built-ins
    and `createBuiltinProcessor(id)` makes one.
  - One adapter wraps every vloom-dsp effect as an `AudioProcessor`. It
    applies parameter changes at their frame, clamps values and ignores NaN.
  - The state format is shared by every built-in ("VLB1" plus id/value
    pairs), and unknown ids are skipped.
- **VST 3 hosting (M5, ADR 0006):** `plugin/Vst3Host`.
  - **Loading:** `Vst3Library::open()` loads a .vst3, as a bundle or a
    single file, and lists its audio plugins as effects or instruments.
  - **Creating:** `create()` wraps a plugin in an `AudioProcessor`. The
    component and edit controller can be one object or two connected
    objects.
  - **Processing:** stereo main buses, normalized parameter changes, notes
    on the event input, the transport as a `ProcessContext`, latency, and
    state.
  - **Finding plugins:** `vst3SearchPaths()` and `findVst3Files()` (with
    `VST3_PATH`).
  - **Dependency:** Steinberg's `vst3_pluginterfaces` v3.8.1_build_84 (MIT),
    pinned to its commit. Only the interfaces are used, not the SDK.
  - **Internal:** the CLAP and VST 3 hosts now share library loading and the
    plugin file search (`src/plugin/PluginFiles`).
- Mixer routing and plugin delay compensation (M5, ADR 0007):
  - `InsertState::output` plays an insert into the master, into another
    insert, or nowhere.
  - `InsertState::sends` sends up to `kMaxSends` copies, pre- or post-fader,
    with ramped gain.
  - A route that would close a loop is ignored.
  - `Engine::setMixerState()` compiles the state into a `MixerGraph` that
    processes inserts before what they play into, and delays every path to
    line up with plugin latency (up to `kMaxLatencyFrames`).
  - The metronome is delayed as much as the music; the preview is not.
  - `Engine::latencyFrames()` reports how late the music comes.
  - `PluginInstance::latencyFrames()`; a bypassed instance delays its dry
    sound by its latency.
  - `exportSong()` leaves out the lead-in.
  - New `dsp/DelayLine`.
- Plugin host interface (M4, ADR 0006): `plugin/AudioProcessor`, the one
  interface every hosted processor implements whatever its format (CLAP and
  VST3 adapters come next), with parameters, per-block parameter changes and
  note events at their frame, transport info, latency and state.
  `plugin/PluginInstance` wraps one for the engine: activation, bypass, and a
  lock-free queue of parameter changes (the latest per parameter at the next
  block; kept while bypassed). Mixer inserts have an effect chain
  (`InsertState::effects`), processed in order before gain and pan, every
  block so tails ring out; a removed effect is destroyed on the control
  thread once the audio thread is done with it, and `Engine::prepare()`
  activates the hosted effects again at the new rate. An inactive or
  bypassed processor passes audio through.
- Built-in effect `plugin/GainProcessor` ("vloom.gain", -60 .. +12 dB,
  ramped): the first `AudioProcessor`. Replaced in M5 by
  `createBuiltinProcessor("vloom.gain")` (ADR 0008).
- CLAP hosting (M4, ADR 0006): `plugin/ClapHost` loads .clap files
  (`ClapLibrary::open()`, once per file), lists their plugins (effect or
  instrument) and creates them as `AudioProcessor`s with parameters, state,
  latency, notes and transport. `clapSearchPaths()` / `findClapFiles()` find
  installed plugins. The CLAP SDK (MIT, headers only) is pinned at 1.2.10.
- Instrument plugins on channels: `Instrument::Plugin` with
  `ChannelState::plugin`. Steps and piano-roll notes reach the plugin as
  sample-accurate note events (`sequencer/PluginNotes`); it is processed once
  per block and mixed like any channel. `Engine::prepare()` activates
  channel plugins again too.
- `PluginInstance::process()` takes note events; `AudioProcessor::idle()`
  for work a plugin asks to run on the main thread. Hosted plugins get the
  transport as at the block's start.
- Audio clips fitted to the song's tempo: `SongAudioClip::sample` replaces
  the channel's sample for that clip (e.g. a copy time-stretched to the
  tempo) and `SongAudioClip::rate` plays it faster or slower (resampled, the
  pitch following); offsets count in the sample as played, and
  `songEndTicks()` follows. An old state is kept while a clip's own sample
  still sounds, as for channel samples (ADR 0005).
- Clips that show part of their content, for cutting and trimming them in a
  playlist: `SongPatternClip::offsetSteps` starts a pattern clip that far
  into its pattern (steps and notes; wrapping round a repeating pattern);
  `SongAudioClip::offsetSteps` starts an audio clip that far into its sample
  (at the current tempo, faded in) and `SongAudioClip::lengthSteps` ends it
  there (faded out over `Sampler::kReleaseFrames`), also when playback starts
  inside it. `songEndTicks()` counts only the part a clip plays.
  `StepSequencer::triggerStep()` takes the frames per tick to place the offset.
- Loop region and stop point (M3): `Engine::setLoop(start, end)` loops
  `[start, end)` while playing, jumping back on the exact sample, with the
  events at the loop start firing every pass (song-mode audio clips under
  way resume part-way, held notes are released, as for a locate);
  `clearLoop()` turns it off, and loops shorter than `kMinLoopTicks` are
  ignored. Playing from past the loop end goes on. `Engine::setStopAt(ticks)`
  stops the transport on the exact sample (e.g. at the song's end), fading
  sounding notes out. Each block is played in spans split at these
  boundaries.
- Song export (M3): `exportSong()` renders a song into a WAV file faster
  than real time with an Engine and voices of its own
  (`copyWithOwnVoices()`), so it can run on a background thread while the
  live engine plays. It ends at the song's end (`songEndTicks()`: the last
  pattern or audio clip) plus the tail until the sound dies away (at most
  `maxTailSeconds`), reports progress and can be cancelled; a cancelled or
  failed export writes no file. Samples must be loaded at the export rate.
- WAV files in 16- and 24-bit integer PCM besides 32-bit float (`WavFormat`),
  clamped to full scale, with optional TPDF dither; `WavFileWriter` takes a
  `Spec` and also writes files of unknown length (the header gets the real
  length in `finish()`). `renderToWav()` and `vloom-cli render --format`
  choose the format.
- A pattern clip longer than its pattern repeats it (steps and notes) when
  the pattern's length is given (`SongPattern::stepCount`), like FL Studio.
- `Transport::kMinBpm` / `kMaxBpm` are public.
- Piano-roll notes (M2): `Note` (start, length in ticks, MIDI key, velocity)
  in `ChannelState::notes` (pattern mode, looping with the pattern) and
  `SongPattern::notes` (song mode, notes starting inside a pattern clip). A
  note starts on its exact sample, is held for its length, then released
  over `Sampler::kReleaseFrames`; a jump (locate) releases held notes. The
  key plays the channel's sample relative to `ChannelState::rootKey`.
- Pitched sampler: `Sampler::play()` with `PlayOptions` (rate, held notes
  via `releaseAt`, `release()`, `nextRelease()`, `releaseHeld()`); rates
  other than 1 read the sample with 4-point Hermite interpolation, rate 1
  stays bit-exact. `setPitch()` scales the voices that follow it, live.
- First built-in synth (`dsp/Synth`): polyphonic subtractive synth with one
  oscillator per voice (sine, triangle, polyBLEP saw and square), a linear
  ADSR envelope and a resonant TPT state-variable low-pass. A channel picks
  it with `ChannelState::instrument = Instrument::Synth` and sets its sound
  in `ChannelState::synth` (`SynthSettings`, live); steps play its root key
  for one step, notes their key. `ChannelVoices` keeps a sampler and a synth,
  so switching instrument lets ringing notes finish.
- Master pitch: `Engine::setMasterPitch(cents)`, ±1200 cents, transposes
  steps and notes (sounding ones too) but not audio clips, which stay in
  time with the song, or the preview.
- Song mode (`SequencerState::song`): the sequencer plays the playlist instead
  of looping the pattern. Pattern clips (`SongPatternClip`) trigger the
  channels with their pattern's steps (`SongPattern`); audio clips
  (`SongAudioClip`) play their channel's sample once from their step, at its
  own speed, through the channel (volume, pan, mute, insert). When playback
  starts or jumps (locate) inside an audio clip, it starts part-way through
  (`StepSequencer::resumeAt()`). Stop fades the clips out.
- `Sampler::trigger()` takes a start frame: a sample can start part-way,
  fading in over `kFadeFrames` so it does not click.
- Sample preview: `Engine::previewSample()` plays a sample once through the
  master bus on its own voice, whether or not the transport is running; a new
  sample replaces the one sounding, null or `stop()` silences it (64-frame
  fade), and `isPreviewPlaying()` reports it. A replaced sample is freed on the
  control thread (`collectGarbage()`) only after its voice has faded out; the
  audio thread does not allocate.
- `DeviceListWatcher` (`io/AudioDevice`): counts output-device changes as the
  OS reports them (WASAPI endpoint notifications, CoreAudio property
  listeners), so callers list devices only after a change instead of polling,
  which takes 50-200 ms on Windows; ALSA reports it cannot notify
- Mixer routing (`mixer/MixerState`): each step-sequencer channel plays into
  one of up to 125 insert buses (or straight into the master) with its own
  gain and balance pan; each insert has gain, pan and a peak meter
  (`Engine::setMixerState()`, `takeInsertPeak()`). Every level change is
  ramped over one block, and blocks longer than 4096 frames are split
- A lost output device behaves the same on every OS: the backend stops sending
  audio and `isRunning()` turns false (CoreAudio's unit is kept silent instead of
  moving to another output on its own); `vloom-cli play` stops with an error
- Audio output through each OS's own API, behind `io/AudioDevice`: WASAPI
  (shared, event driven, MMCSS "Pro Audio") on Windows, CoreAudio (AUHAL) on
  macOS, ALSA on Linux (also reaching PulseAudio and PipeWire)
- Own 32-bit float WAV writer (`render/WavWriter`)
- WAV reader for untrusted files: PCM 8/16/24/32-bit, float 32/64-bit,
  WAVE_FORMAT_EXTENSIBLE; bounds-checked chunk walk, metadata skipped
  unread, size limits before allocating, NaN / infinity become silence
  (`sample/WavReader`)
- Band-limited windowed-sinc resampler (`sample/Resampler`) and
  `loadSample()` to read a file at the engine's rate
- One-shot sample player with per-channel polyphony (1-64 voices) that fades
  stolen and stopped notes instead of clicking (`dsp/Sampler`)
- Lock-free snapshot hand-over between the control and audio threads
  (`core/SnapshotExchange`, ADR 0005)
- Step sequencer: channels with sample, gain, pan, mute and steps, played
  sample-accurately alongside the metronome; edits never cut ringing notes,
  removed channels and transport stop fade out (`sequencer/`,
  `Engine::setSequencerState`)
- `vloom-cli play` / `render` take `--channel <file.wav>:<pattern>` and
  `--click` / `--no-click`
- Wait-free command queue from control thread to audio thread (`core/SpscQueue`)
- Sample-accurate, drift-free transport at 960 PPQ (`time/Transport`)
- Metronome (`dsp/Metronome`)
- Master gain with a per-block ramp (`Engine::setMasterGain`)
- Smoothed CPU load of the audio callback (`Engine::cpuLoad`)
- Master balance with the same ramp as the gain (`Engine::setMasterPan`)
- Lock-free output peak metering for the UI (`Engine::takeOutputPeak`)
- Offline rendering to WAV (`render/OfflineRenderer`)
- `vloom-cli` with `devices`, `play` and `render` commands

### Removed
- miniaudio. It was used only for device output and WAV writing, but its
  whole source, decoders included, was built in; 0.11.21 carried
  CVE-2024-41147 (heap overflow in the FLAC decoder). See ADR 0004.
