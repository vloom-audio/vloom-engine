# Architecture

## Modules

| Module | Folder | Responsibility | May depend on |
|---|---|---|---|
| engine | `Engine.*` | Ties modules together; owns the audio callback | all modules |
| core | `core/` | Lock-free queue, version, small utilities | — |
| time | `time/` | PPQ, bar/beat/tick, transport clock | core |
| dsp | `dsp/` | Sound generators and processors | core |
| io | `io/` | Hardware audio devices (WASAPI, CoreAudio, ALSA) | core |
| mixer | `mixer/` | Insert settings (volume, pan) and the pan law; Engine mixes the buses | core |
| sample | `sample/` | Decoded audio in memory: WAV reading, resampling | core |
| sequencer | `sequencer/` | Channel rack state and step playback | core, dsp, sample, time |
| render | `render/` | Offline rendering, file export | engine |

Rules:

- Lower modules never include higher ones (`dsp` never includes `Engine.hpp`).
- Platform audio APIs are used only in `src/io/<Name>Backend.cpp`, one file per
  OS behind the internal `io::AudioBackend` interface. No platform header
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
5. **M3** arrangement (patterns on a timeline), song mode, WAV export ← *in progress*
   (song mode, pattern clips repeating their pattern, whole-song export to
   float / 16 / 24-bit WAV are in; a loop region is next)
6. **M4** plugin host interface, CLAP hosting
7. **M5** VST3 hosting, built-in effects, sends, delay compensation
8. **M6** audio input and recording, MIDI input
