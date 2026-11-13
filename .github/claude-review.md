# Review guide for Claude (vloom-engine)

vloom-engine is a Qt-free, real-time audio engine in C++20. Read
`docs/architecture.md`, `docs/coding-guidelines.md` and the ADRs in
`docs/adr/` when a change touches their area.

## What matters here

1. **The audio thread is sacred.** Anything reachable from `Engine::process()`
   (and device callbacks) must not allocate or free (`new`, `push_back`,
   `std::string`, `std::function` copies, a `shared_ptr` refcount dropping to
   zero), lock, wait, log, do I/O or throw. Check every new call on that path.
2. **Threading contract.** One control thread calls the setters; the audio
   thread calls `process()`; status getters may be called from any thread and
   must be atomic. Look for plain fields shared between threads, memory
   ordering in `SpscQueue` / `SnapshotExchange`, and state changed in
   `prepare()` while a callback could still run.
3. **Lifetimes (ADR 0005).** Snapshots are freed on the control thread only
   after the audio thread is done with them, including samples that voices
   still read through raw pointers (fades, draining states, the preview).
   `ChannelVoices` belong to one Engine.
4. **Sample-accurate scheduling.** Events fire once, on the right sample,
   across block boundaries, tempo changes, locate, stop/play and pattern
   changes. Watch for off-by-one, rounding at block edges and events that can
   be fired twice or skipped.
5. **Untrusted input.** WAV files, `SequencerState` / `MixerState` values from
   the host and CLI arguments: bounds, integer overflow (`int64` tick maths),
   NaN / infinity, huge allocations, division by zero. Undefined behaviour
   counts as a bug (CI runs UBSan with `-fno-sanitize-recover`).
6. **DSP.** Clicks (missing fades or ramps), NaN or denormals, out-of-bounds
   interpolation reads, voices that never end, aliasing in new oscillators.
7. **Platform I/O.** WASAPI / CoreAudio / ALSA resource cleanup on every error
   path, callbacks after stop or destruction, lost-device handling.
8. **Tests.** Every bug fix needs a test that fails without it; DSP tests render
   offline and check exact samples. New code paths on the audio thread should
   be covered by an allocation-counting test.

## Conventions (only mention real violations)

- Public API changes need `CHANGELOG.md`; architecture changes need an ADR.
- Commits: `<gitmoji> <type>(<scope>): <subject>`. clang-format is enforced by
  CI: do not comment on formatting. English everywhere.

## How to report

- Only verified problems, most severe first. For each: the file and line, a
  concrete scenario that goes wrong and a short suggested fix.
- Say "Must fix" or "Should fix". Skip style nits, speculation and praise.
- If the PR is fine, say so in one line.
