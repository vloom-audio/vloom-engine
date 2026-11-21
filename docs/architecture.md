# Architecture

## Modules

| Module | Folder | Responsibility | May depend on |
|---|---|---|---|
| engine | `Engine.*` | Ties modules together; owns the audio callback | all modules |
| core | `core/` | Lock-free queue, version, small utilities | — |
| time | `time/` | PPQ, bar/beat/tick, transport clock | core |
| dsp | `dsp/` | Sound generators and processors | core |
| io | `io/` | Hardware audio devices, output and input (WASAPI, ASIO, CoreAudio, ALSA), and MIDI inputs (WinMM, CoreMIDI, ALSA sequencer) | core |
| mixer | `mixer/` | Insert settings (volume, pan, effect chain, output, sends), the pan law, and `MixerGraph`: inserts in processing order with delay compensation (ADR 0007); Engine mixes the buses along it | core, dsp, plugin |
| plugin | `plugin/` | Hosted processors behind one interface (ADR 0006): CLAP, VST 3 and VST 2 (ADR 0010) hosts, built-in processors from vloom-dsp (ADR 0008) | core, time |
| sample | `sample/` | Decoded audio in memory: WAV reading, resampling | core |
| sequencer | `sequencer/` | Channel rack state and step playback (built-in and plugin instruments) | core, dsp, plugin, sample, time |
| record | `record/` | Recordings put together on the control thread: `AudioTake` writes the engine's recorded input to a file and places it in the song (ADR 0016), punch stretches included (ADR 0017) | engine, render |
| render | `render/` | Offline rendering, file export, bouncing a pattern to audio (ADR 0018) | engine |

Rules:

- Lower modules never include higher ones (`dsp` never includes `Engine.hpp`).
- Platform audio and MIDI APIs are used only in `src/io/<Name>Backend.cpp`
  (audio output, MIDI input) and `src/io/<Name>InputBackend.cpp` (audio input),
  one file per OS behind the internal `io::AudioBackend`, `io::InputBackend` and
  `io::MidiBackend` interfaces. No platform header
  appears in `include/` (see [ADR 0004](adr/0004-native-audio-io.md)).
- Each public header in `include/vloom/engine/<module>/` has its `.cpp` in
  `src/<module>/` and its tests in `tests/<module>/`.

## Threads

| Thread | Owns | Rules |
|---|---|---|
| Control (UI) | project data, undo history | may allocate, lock, do I/O |
| Audio (real-time) | `Transport`, DSP state | no allocation, no locks, no I/O, bounded time |
| Background (later) | sample loading, plugin scanning, graph building | hands finished objects to the audio thread |

## Control → audio

`Engine::play()`, `stop()`, `setTempo()` … push a small, trivially copyable
`Command` into a wait-free single-producer / single-consumer ring buffer
(`core/SpscQueue.hpp`). `Engine::process()` drains it at the start of every block.

Structured state (channels, samples, patterns) does not use this queue. The
control thread builds an immutable snapshot and publishes it with
`core/SnapshotExchange`: one atomic pointer exchange. Replaced snapshots drain
until no voice plays their samples, then go back to the control thread to be
freed. Voice state lives outside the snapshots, so edits never cut sound.
See [ADR 0005](adr/0005-state-snapshots-for-the-audio-thread.md).

## Audio → control

After each block the engine publishes play state, playhead and tempo to
`std::atomic` values. The UI polls them on a timer (30–60 Hz).

## Time

- Musical time is in ticks, 960 per quarter note (`kPpq`, `time/MusicalTime.hpp`).
- `Transport` stores an *anchor* (ticks at the last tempo change or locate)
  and counts whole frames since then. Position is always derived from those,
  so rounding error never accumulates.
- Events (metronome clicks, channel-rack steps, note starts and note ends)
  are scheduled on the exact sample they fall on: the block is rendered in
  segments split at each event's offset. A note's end is kept by its voice
  (`Sampler::PlayOptions::releaseAt`, in ticks), so it ends on time even when
  the pattern loops or is edited meanwhile.

## Testing

Anything that does not need hardware is tested offline. `renderInterleaved()`
drives `Engine::process()` faster than real time and tests check
sample-exact results across many block sizes. Rendering must be deterministic.

## Roadmap

1. **M0** device output, transport, metronome, offline render
2. **M1** pattern / step sequencer, sample playback (drum channel rack), mixer routing
3. **M1.5** sample preview voice ✅
4. **M2** MIDI notes, pitched sampler, first built-in synth, master pitch ✅
   (engine side; the app's piano roll and synth panel are next)
5. **M3** arrangement (patterns on a timeline), song mode, loop region, WAV export ✅
   (engine side; the app's export dialog and loop markers are next)
6. **M4** plugin host interface, CLAP hosting ✅ (ADR 0006: `AudioProcessor`,
   insert effect chains, CLAP effects and instruments)
7. **M5** VST3 hosting, built-in effects, sends, delay compensation ✅
   VST 3 (and VST 2, ADR 0010) hosting, sends and sidechains, delay
   compensation (ADR 0007), and the 20 built-in processors from vloom-dsp
   (ADR 0008): 17 effects (compressor, limiter, gate, de-esser, transient
   shaper, EQ, filter, delay, reverb, chorus, flanger, phaser, tremolo,
   saturator, bitcrusher, gain, utility) and the synth, sampler and drum
   sampler
8. **M6** audio input and recording, MIDI input ✅ (ADR 0015: MIDI input and
   recording; ADR 0016: audio input, monitoring and recording)
9. **v1.0** engine work ✅ (engine side; the app's controls are next):
   count-in and punch-in / punch-out for audio and MIDI takes (ADR 0017),
   bouncing a pattern or part of the song to audio (ADR 0018)
