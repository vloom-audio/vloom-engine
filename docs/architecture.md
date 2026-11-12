# Architecture

## Modules

| Module | Folder | Responsibility | May depend on |
|---|---|---|---|
| engine | `Engine.*` | Ties modules together; owns the audio callback | all modules |
| core | `core/` | Lock-free queue, version, small utilities | — |
| time | `time/` | PPQ, bar/beat/tick, transport clock | core |
| dsp | `dsp/` | Sound generators and processors | core |
| io | `io/` | Hardware audio devices (WASAPI, CoreAudio, ALSA) | core |
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

Structural changes (adding a channel, inserting an effect) will not use this
queue. A background thread will build a new immutable *render graph* and the
audio thread will switch to it with one atomic pointer exchange; the old graph
is freed later on a non-real-time thread.

## Audio → control

After each block the engine publishes play state, playhead and tempo to
`std::atomic` values. The UI polls them on a timer (30–60 Hz).

## Time

- Musical time is in ticks, 960 per quarter note (`kPpq`, `time/MusicalTime.hpp`).
- `Transport` stores an *anchor* (ticks at the last tempo change or locate)
  and counts whole frames since then. Position is always derived from those,
  so rounding error never accumulates.
- Events (today: metronome clicks) are scheduled on the exact sample they fall
  on: the block is rendered in segments split at each event's offset.

## Testing

Anything that does not need hardware is tested offline. `renderInterleaved()`
drives `Engine::process()` faster than real time and tests check
sample-exact results across many block sizes. Rendering must be deterministic.

## Roadmap

1. **M0** device output, transport, metronome, offline render ← *here*
2. **M1** pattern / step sequencer, sample playback (drum channel rack)
3. **M2** MIDI notes, piano-roll data model, first built-in synth
4. **M3** playlist (patterns on a timeline), mixer, WAV export
5. **M4** CLAP then VST3 hosting, project save / load
