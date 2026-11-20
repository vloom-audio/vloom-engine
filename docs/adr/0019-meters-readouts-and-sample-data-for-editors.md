# 0019. Meters, readouts and sample data for editors

- Status: accepted
- Date: 2026-11-21

## Context

VloomDAW draws its own editors for the built-in processors (ADR 0008) and
a generic one for any plugin. Before this change those editors had to guess
or poll:

- They had no input or output level per instance. The UI estimated levels
  from the scope, which is mono and only shows the output.
- The scope (`PluginInstance::readScope()`) averages left and right, so a
  UI cannot draw a vectorscope, a correlation meter or L/R levels.
- The only processor value besides parameters was `gainReductionDb()`. A
  gate's open state, which vloom-dsp's Gate already reports, could not
  reach the UI.
- A sampler UI could see a slot's path, channels and frame count, but not
  its audio. To draw a waveform it had to read the file again, and it had
  to compare paths on a timer to notice a change.

All of this is read while the audio thread processes. The audio thread must
not allocate, lock or wait (ADR 0006), and the UI must not block it.

## Decision

1. **Level meters per instance, at `PluginInstance` level.** They work for
   every processor: CLAP, VST 3, VST 2 and built-in.
   - `setMetersEnabled(bool)` switches them on or off. Off is the default
     and costs one relaxed load per block. Switching on starts from
     silence. The meters allocate nothing: four atomics live in the
     instance.
   - The audio thread raises the input peaks (L, R) before the processor
     runs, because the processor works in place. It raises the output
     peaks after the processor, on the main output, together with the
     scope. When the instance is bypassed, the input is the dry sound and
     the output is the delayed dry sound. An inactive instance measures
     nothing.
   - A peak is the largest absolute sample, linear (1 = 0 dBFS); NaN is
     skipped. The audio thread raises each peak with a compare-exchange
     loop. That loop is lock-free, and it only retries while the reader
     swaps the peak out between the load and the store.
   - `readLevels()` swaps every peak with 0 and returns
     `Levels{input[2], output[2]}`. So each read gives the peak since the
     last read, and a UI polling at any rate sees every peak exactly once.
     It is meant for one reader: two readers would split the peaks between
     them.
2. **A stereo scope.** The scope ring now keeps left and right
   interleaved (2 × `kScopeFrames` atomics, allocated on the first
   `setScopeEnabled(true)` as before).
   - `readScopeStereo(left, right)` returns the newest
     min(left.size(), right.size()) frames of each channel, frame-aligned.
   - `readScope(out)` keeps its signature and its result. It computes the
     average of left and right when it reads, where the audio thread used
     to compute it when it wrote.
3. **Generic readouts on `AudioProcessor`.**
   - `readouts()` returns `ReadoutInfo{id, name, unit, toggle}`. The list
     is fixed per processor, and it is meant for the control thread.
   - `readoutValue(index)` is `noexcept` and safe on any thread. It returns
     0 for a bad index.
   - `toggle` marks a state, whose value is 0 or 1, as opposed to an
     amount.
   - Ids are stable strings, so a UI can recognise a readout without
     relying on its index.
   - `gainReductionDb()` stays for compatibility. For the built-ins it
     reads the same value as the "gainReduction" readout.
   - Hosted plugins report no readouts for now.
4. **One mapping table in the built-in adapter.** `builtin::readoutTable<E>()`
   (in `src/plugin/builtin/BuiltinEffect.hpp`) is the only place where a
   vloom-dsp capability becomes a readout. It is a `constexpr` array of
   `{id, name, unit, toggle, value(const E&) noexcept}`, built with
   `if constexpr` on concepts:
   - `GainReductionMeter` (`gainReductionDb()`) gives "gainReduction" /
     "Gain Reduction" / "dB". The compressor, gate, limiter and de-esser
     have it.
   - `OpenStateMeter` (`isOpen()`) gives "gateOpen" / "Gate Open", a
     toggle. The gate has it.

   `readouts()` and `readoutValue()` only walk this table, so a value is a
   single indirect call that reads an atomic in the effect.
5. **Sample data for the UI.**
   - `AudioProcessor::sampleAudio(slot)` returns
     `std::shared_ptr<const SampleAudio>`, a read-only view with channels,
     frames, rate and one `std::span<const float>` per channel. It returns
     null for an empty slot or a bad slot.
   - It reuses `loadSample()`'s ownership model. The view aliases the
     same immutable `dsp::SampleBuffer` the processor plays, and its
     control block holds a `shared_ptr` to it. So the view costs no copy
     of the audio, and it stays valid and unchanged after a load replaces
     the slot.
   - The audio thread never touches a reference count. vloom-dsp's
     `SampleSlot` hands it raw pointers guarded by hazard marks, so the
     last release happens on whichever UI or control thread drops the
     view.
   - `sampleAudio()` reads `dsp::SampleSlots::sample()` under the
     adapter's slot mutex, which only host-side threads take. So it is
     "any thread but the audio thread", and it never throws.
   - `sampleSlotsVersion()` is an atomic counter, lock-free on any thread.
     It is bumped, with release ordering and under the slot mutex, after
     every successful `loadSample()` and `clearSample()`, and after every
     slot that `loadState()` could not load. A failed `loadSample()` leaves
     it alone. A UI polls the counter and calls `sampleSlot()` /
     `sampleAudio()` only when it changes. It reads the counter first, so a
     change made between the two calls shows up on the next poll.

## Plugging in vloom-dsp's readouts later

vloom-dsp is adding a `HasReadouts` concept (`readoutSpecs()` and
`readout(i)`), which is not merged yet. To support it, the engine will:

1. Bump the vloom-dsp pin.
2. In `readoutTable()`'s place, list the table's entries first and then
   the effect's `readoutSpecs()`. `readoutValue(i)` reads
   `effect_.readout(i - kReadoutCount)` past the table.
3. Drop a table line for an effect that reports the same value through
   `HasReadouts`, using a `!HasReadouts<E>` condition or a match on the
   id, so no id is listed twice. Ids from vloom-dsp are passed through
   unchanged. "gainReduction" and "gateOpen" should keep those ids in
   vloom-dsp, so a UI does not notice the move.

The public API (`ReadoutInfo`, `readouts()`, `readoutValue()`) does not
change.

## Alternatives considered

- **Meters inside each processor (or each host).** CLAP, VST 3, VST 2 and
  built-in would each need their own code. Measuring around
  `AudioProcessor::process()` in `PluginInstance` covers them all.
- **RMS or a ballistic meter in the engine.** A peak since the last read
  is the raw data. The UI applies its own ballistics (fall rate, peak
  hold) and can compute RMS from `readScopeStereo()`.
- **A peak ring the audio thread pushes into (SPSC).** That needs a size
  and a policy for when it is full. A swapped atomic max loses nothing and
  costs nothing while no one reads it.
- **A channel-selecting `readScope(out, channel)`.** Two separate reads
  would not be frame-aligned, which a vectorscope needs.
- **`readoutValue()` returning `std::optional<float>`.** The list is fixed,
  so a bad index is a caller bug. Returning a plain 0 keeps polling cheap.
- **Exposing `dsp::SampleBuffer` or copying into `vloom::SampleBuffer`.**
  Engine headers do not include vloom-dsp (ADR 0008), and a copy doubles a
  long sample's memory on every poll. Precomputed peaks would fix the
  zoom levels for the UI. A view of the real samples lets the UI decide.

## Consequences

- An open editor turns on meters and the scope for its instance only, so
  idle instances still cost nothing.
- The scope's memory doubles (2 × 8192 floats, 64 KiB) for an instance
  whose scope is on.
- A UI that holds a `SampleAudio` keeps a replaced sample in memory until
  it lets go. The UI should drop the view when `sampleSlotsVersion()`
  changes.
- `readLevels()` assumes one reader per instance. VloomDAW reads from its
  UI timer.
- Not yet: readouts from hosted plugins (CLAP has no such extension), and
  meters on a plugin's extra outputs (ADR 0012).
