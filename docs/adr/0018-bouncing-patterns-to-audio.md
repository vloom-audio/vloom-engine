# 0018. Bouncing a pattern to audio

- Status: accepted
- Date: 2026-11-20

## Context

v1.0 lets the user turn a pattern clip into an audio clip ("render to
audio", "bounce in place"): to free a plugin's CPU, to freeze a sound, or
to edit it as audio. The engine already renders a song offline with an
Engine of its own (`exportSong()`, M3), so the live engine and its device
are untouched. What is missing is rendering one pattern, alone, with a
choice of channels and of how much of the mixer to go through, into
memory or a file the app can put in an audio clip.

## Decision

1. **`bouncePattern(state, pattern, mixer, sampleRate, options)`** renders
   one pass of a pattern from its first tick, whatever the playlist holds.
   It builds a song of its own: that pattern as one clip at tick 0, as long
   as the pattern (`patternLengthTicks()`: its `stepCount` steps), and
   renders it through the export's path. `kChannelRackPattern` bounces the
   channel rack's own pattern (pattern mode) the same way.
   **`bounceRange(song, start, end, ...)`** does the same for part of the
   song, audio clips included.
2. **The same renderer as the export.** `exportSong()` and the bounces
   share `detail::renderSong()`: an Engine and voices of its own, the tempo
   and master pitch given, the delay compensation's lead-in left out, and
   the tail rendered until 50 ms of silence or `maxTailSeconds` (0: exactly
   the pattern's length). So a bounce sounds exactly like the pattern
   played by the engine, sample for sample. The rate is the one given (the
   project's); samples must already be at it.
3. **Options** (`BounceOptions`):
   - `channels`: the channels heard; the others are muted for the bounce.
   - `mix`: `Dry` (each channel's instrument, volume and pan, no insert and
     no master effect: for an audio clip that plays through the same
     channel and insert again), `Inserts` (the default: through the
     channels' inserts, sends and routes into the master bus, before the
     master's effects, volume and pan: for an audio clip that plays into the
     master), or `Master` (the whole mixer, as an export).
   - `masterPitchCents`: the channel rack is transposed by the master pitch
     and an audio clip is not, so the bounce carries it.
   - `path`: a stereo WAV file in `format`; empty: interleaved stereo floats
     in `BounceResult::audio`. `bodyFrames` and `lengthTicks` give the
     pattern's own length, without the tail, for the clip.
4. **Off the audio thread, as an export.** A bounce runs on the calling
   thread and blocks until done, cancelled (the same progress callback as
   the export) or failed. The live engine and its voices are not touched.
   Plugin instances are shared with the live engine, though, like an
   export's: the app stops the device while it bounces a pattern with
   plugins, as VloomDAW already does for an export.

## Alternatives considered

- **Bouncing on the live engine in real time**: no plugin sharing issue,
  but it takes as long as the pattern and the user hears it.
- **Freezing in the live engine** (rendering a channel's output into a
  buffer while the song plays): fits a "freeze track" feature, not
  replacing a clip; it would also need the whole pattern played first.
- **Cloning plugin instances for the bounce** so the device can keep
  playing: needs state save and restore for every format, and some plugins
  (licence checks, big samplers) do not take well to a second instance.
  Left for later.

## Consequences

- The app can replace a pattern clip with an audio clip of the same length
  (`lengthTicks`) at the same place, routed as `mix` says.
- `exportSong()` behaves as before; its body moved into the shared
  renderer.
- Channels are left out by muting them for the bounce; its voices start
  silent, so nothing of theirs is heard.
- A bounce with plugins pauses the output, as an export does.
