# Changelog

All notable changes to vloom-engine are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Security
- Supply chain: GitHub Actions are pinned to commit SHAs (checkout v7.0.1, cache
  v6.1.0; Dependabot keeps them current), the CPM.cmake download is checked against a
  pinned SHA-256 (a cached copy that does not match is fetched again), and Catch2 is
  pinned to the commit of its v3.7.1 tag.

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
  ramped): the first `AudioProcessor`.
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
