# 0008. Built-in processors from vloom-dsp

- Status: accepted
- Date: 2026-11-14 (amended 2026-11-20: all 20 processors, optional
  capabilities, sample slots)

## Context

M5 adds built-in effects, and more built-in effects and instruments are
planned. Some of them should also ship as stand-alone CLAP or VST 3 plugins,
and some will be sold.

Written inside the engine, that code would be GPL (ADR 0003) and tied to the
engine's types, so it could not be built as a plugin or used under another
license. The engine also has to stay buildable from source by anyone, so it
cannot depend on closed code.

## Decision

1. **The sound lives in vloom-dsp.** It is our own MIT-licensed library
   (github.com/vloom-audio/vloom-dsp) that depends only on the C++ standard
   library. It describes an effect with the `dsp::Effect` concept:
   - a `ProcessorSpec`, the stable id, name and version;
   - a list of `ParameterSpec`;
   - `prepare()`, which sizes memory;
   - `setParameter()`, `process()`, `reset()` and `latencyFrames()`, which are
     real-time safe.
2. **The engine pulls it in with CPM, pinned to a commit**
   (`cmake/Dependencies.cmake`), and links it privately. Engine headers do
   not include vloom-dsp.
3. **One adapter wraps any effect.** `builtin::BuiltinEffect<E>`
   (`src/plugin/builtin/`) implements `AudioProcessor`, so built-ins plug into
   inserts, bypass and delay compensation like hosted plugins (ADR 0006):
   - it splits the block at each parameter change's frame, so changes are
     sample-accurate;
   - it clamps values to their range, rounds stepped ones and ignores NaN;
   - it keeps the latest value of each parameter in an atomic, so
     `saveState()` on the control thread sees what was applied;
   - `loadState()` hands values to the audio thread for its next block, and
     values loaded before `activate()` apply without a glide;
   - it reads the latency at activation.
4. **The state format is shared by every built-in:** "VLB1", a 4-byte count,
   then per parameter its 4-byte id and 8-byte value, all little endian.
   Unknown ids are skipped, so state saved by a newer version still loads.
5. **Built-ins are found by id.** `builtinProcessors()` lists them, and
   `createBuiltinProcessor(id)` makes one. Ids such as "vloom.gain" are
   stable, because saved projects refer to them.
6. **Optional capabilities are found at compile time** (vloom-dsp ADR 0002
   and 0003). The adapter checks each with `if constexpr`; a processor
   without one is wrapped as before.
   - `dsp::SidechainEffect` (compressor, gate): `hasSidechain()` is true, so
     the mixer's sidechain sends reach it like a plugin's second input,
     delay-compensated the same way (ADR 0007, 0012). While a key arrives
     (`ProcessContext::sidechain`) the adapter calls
     `processWithSidechain()` with both key channels, split at the same
     frames as the sound; otherwise `process()`.
   - `dsp::TempoAware` (delay, chorus, flanger, phaser, tremolo): the
     adapter calls `setTempo()` with `TransportInfo::tempo` before the first
     block after `activate()` and before any block whose tempo differs from
     the last one given.
   - Latency that changes with a parameter (limiter: Lookahead, True Peak;
     saturator: Oversampling): the audio thread reads `latencyFrames()`
     again after every block that applied a change and keeps it in an
     atomic, which `AudioProcessor::latencyFrames()` returns. The host's
     `Engine::updatePluginLatency()` (a UI timer) then lines the mixer up
     anew, as for a plugin.
   - Meters: `gainReductionDb()` (compressor, gate, limiter, de-esser) as
     `AudioProcessor::gainReductionDb()`, any thread.
   - `dsp::SampleSlots` (sampler: 1 slot, drum sampler: 16): see 8.
7. **Choices have labels.** `ParameterInfo::valueLabels` names each whole
   value of a stepped choice ("Peak", "RMS"; note lengths from
   `dsp::kNoteValues` or the delay's own list; MIDI keys as "C3" = 60). The
   registry gives each built-in a function for its labels; a list that does
   not name every value is dropped, so a host never shows a wrong name.
   Switches (0 .. 1 without labels) stay switches. The registry also gives
   each built-in a `PluginInfo::category`.
8. **Sample slots.** A sample is not a parameter, so `AudioProcessor` has a
   small non-real-time API: `sampleSlotCount()`, `sampleSlot()`
   (`SampleSlotInfo`: name, path, channels, frames, rate, error),
   `loadSample(slot, path)`, `clearSample()` and `setSampleFileResolver()`.
   - The engine reads the file with its WAV reader, mixes more than two
     channels down to stereo and hands vloom-dsp a `SampleBuffer` at the
     file's own rate (the processor resamples as it plays). A host that
     decodes other formats (VloomDAW: MP3, FLAC, ... into cached WAV
     copies) sets a resolver that maps the path it keeps to the WAV file to
     read; the slot keeps the original path.
   - Host side only, one call at a time per processor; a mutex in the
     adapter serializes `setSample()`, `collectGarbage()` and `prepare()` as
     vloom-dsp requires. The audio thread never takes it. Files are decoded
     outside the lock. `idle()` (the host's UI timer) calls
     `collectGarbage()`, so replaced samples are freed on that thread, never
     on the audio thread.
   - The state of a built-in with slots is the parameters ("VLB1" as in 4)
     followed by "SMP1", a 4-byte count and per slot with a file its 4-byte
     index, 4-byte length and UTF-8 path (at most 32 KiB). Unknown slots are
     skipped; anything cut short or left over refuses the state. A state
     without the part empties every slot. Built-ins without slots save
     exactly what they did before and still refuse trailing bytes.
   - `loadState()` loads each file again. One that is missing or cannot be
     read leaves its slot silent with the path kept and the reason in
     `SampleSlotInfo::error`; the state itself loads, and saving it again
     keeps the path, so a project opened on another machine does not lose it.
9. **Closed plugins stay out of the engine.** Processors that are only for
   sale live in a separate closed repository. VloomDAW loads them as CLAP or
   VST 3 plugins through the hosts, never by linking them.

## Alternatives considered

- **Effects inside the engine.** Simpler for now, but GPL and tied to the
  engine.
- **A virtual interface in vloom-dsp instead of a concept.** That costs a
  virtual call per block for every effect. The concept keeps the effect's
  code inlined into the adapter and still checks the shape at compile time.
- **Per-effect state formats.** These would need more code per effect for no
  gain while the state is just parameters. An effect with more state can get
  a versioned extension later.

## Consequences

- `GainProcessor` is gone. "vloom.gain" now comes from vloom-dsp, through
  `createBuiltinProcessor("vloom.gain")`. Its changes glide over 5 ms instead
  of over the block they arrive in, so the glide no longer depends on the
  block size.
- Changing an effect and the engine together takes a vloom-dsp PR first and
  then a pin bump here.
- Built-in instruments are wrapped like effects (`dsp::Instrument`: notes
  at their frames).
- Paths in a state are as the host gave them (absolute, in VloomDAW): a
  project moved to another machine reports its samples as missing until the
  files are found again.
- A sample loaded while the device runs is picked up with the next block;
  a slot replaced twice before then drops the middle one unheard.
- Not yet: the transport position for tempo-synced LFOs (they run free).
  A slot's audio for a waveform view is exposed as `sampleAudio()`, and the
  meters as readouts (ADR 0019).
