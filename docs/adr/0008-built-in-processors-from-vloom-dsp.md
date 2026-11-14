# 0008. Built-in processors from vloom-dsp

- Status: accepted
- Date: 2026-11-14

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
6. **Closed plugins stay out of the engine.** Processors that are only for
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
- Not yet: built-in instruments (a `dsp::Instrument` concept with notes),
  and effect state beyond parameters.
