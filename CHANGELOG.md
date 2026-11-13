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

### Added
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
