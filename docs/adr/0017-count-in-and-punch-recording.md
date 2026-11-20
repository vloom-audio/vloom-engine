# 0017. Count-in and punch recording

- Status: accepted
- Date: 2026-11-20

## Context

v1.0 needs the two recording aids every DAW has. A **count-in** gives the
player a bar or two of clicks before the take starts. A **punch range**
records only one stretch of the song, so a good take can be fixed in one
place, often in a loop, pass after pass.

Both must keep what ADR 0015 and ADR 0016 already guarantee: everything is
placed where the player *heard* the song, with the latency taken off, and
the audio thread does not allocate, lock or wait. The punch edges must be
exact to the sample, even where they fall inside a block, and stay right
when the latency carries a stretch over the loop's jump back.

## Decision

1. **The count-in stands the song still.** `Engine::setCountIn(bars)`
   (0 .. 4). When `play()` starts the song from stop while a recording is
   armed (`setRecording()` or `setAudioRecording()` on), the song's
   transport does not move. A clock of its own counts `bars` bars at the
   song's tempo, and the metronome clicks each beat, the first of each bar
   accented. Then the song starts at its position, on the sample the next
   bar would start (the scheduler's rule: the first sample at or after the
   exact time), and the block goes on from there.
   - The same at bar 1 as anywhere else: the position never goes negative,
     and the song is not heard during the count-in.
   - `setCountInClick(true)` (the default) clicks even with the metronome
     off; false: it follows the metronome and may be a silent wait.
   - `setBeatsPerBar()` sets a bar's length (beats are quarter notes, as
     the metronome counts them) and is passed on to hosted plugins.
   - `isPlaying()` stays false meanwhile; `isCountingIn()`,
     `countInBarsLeft()` and `countInBeatsLeft()` tell the UI. Stop
     cancels it; a locate moves where the song will start.
   - **Nothing is recorded during it.** MIDI played then is played live but
     not recorded. Audio: the input that arrives in the first `shift` frames
     after the song starts was heard during the count-in (the round trip,
     the plugins' latency, the input waiting), so it is left out; the take's
     first frame is the one heard on the song's first sample. MIDI heard
     just before the start because of the latency is placed on the start, so
     a downbeat played a hair early is kept.
2. **Punch range.** `Engine::setPunch(start, end)` in ticks, independent of
   the loop. While it is set:
   - **MIDI**: a message heard inside `[start, end)` (the same heard
     position as ADR 0015, loop pass included) is recorded; outside it is
     only played. Note-offs are always recorded, so a note begun inside can
     end. `MidiTake::setPunch()` applies the note rule: a note whose key
     went down before the punch-in, or at or after the punch-out, is left
     out; a note held over the punch-out ends there; controls outside are
     left out.
   - **Audio**: the engine writes only the input frames heard inside the
     range. A frame is heard `shift` frames before the engine rendered its
     moment, so the range is moved by the shift on the engine's time line;
     just after the loop jumps back that falls in the pass before, as for
     MIDI. The block is played in spans that end where the loop jumps, the
     song stops or the count-in ends; in each span the position moves
     linearly, so the edges are found by arithmetic, on their exact
     samples. Each stretch kept starts with a new `Punch` mark that holds
     where that frame was heard: no shift is applied to it later.
     `AudioTake::segments()` turns each stretch into a segment, so in a
     loop every pass gives one segment of the range. A stretch the latency
     carries over the loop's jump stays one segment.
   - Switching the punch range on or off applies to the audio take from its
     next start; moving the range applies at once.
3. **Recording follows the block's spans.** `processBlock()` logs each span
   (frames, position, tempo, whether the song rolls, whether the loop jumps
   after it) into a fixed array allocated with the engine, and
   `recordInput()` walks it after the mix. Without a punch range the marks
   are the ones ADR 0016 made; only a take that starts after a count-in
   starts mid-block.

## Alternatives considered

- **Pre-roll: starting the song N bars before the record position.** The
  player hears the music leading in, which is nicer, but at bar 1 there is
  nothing to play before it, so the engine would need negative positions
  everywhere (the scheduler, clips, plugins' transport) or two behaviours.
  A count-in that stands the song still behaves the same everywhere. A
  pre-roll can still be built by the app: locate earlier, play, and punch
  in at the record position.
- **Recording everything and cutting the punch range out afterwards** in
  `AudioTake`: simpler on the audio thread, but the file and the ring would
  hold audio the user did not want recorded, and MIDI outside the range
  would still reach the take.
- **Ending held notes on the audio thread** with a note-off at the
  punch-out: the engine would have to track every recorded key per
  channel; `MidiTake` already does, on the control thread.

## Consequences

- The app gets count-in and punch with a few calls, in the same command
  style as the loop: `setCountIn()`, `setCountInClick()`, `setBeatsPerBar()`,
  `setPunch()`, and `MidiTake::setPunch()` with the same range.
- **The app must not read `isPlaying() == false` during a count-in as a
  stop**: check `isCountingIn()`.
- A locate or a tempo change during a punched take places the next
  `shift` frames as if the new position had always been heard: the punch
  is meant for a song playing on or looping.
- Input that arrives after a stop is not kept in a punched take (where it
  was heard is not known).
- At most 1024 spans a block are logged; that many loop jumps in one block
  do not happen with real loops (the shortest loop is a 64th note), and
  beyond it the rest of the block counts as lost.
- The tests render offline and open no device: count-in lengths at several
  tempos, time signatures, rates and block sizes; punch edges to the
  sample with and without latency, in a loop and over its jump; MIDI rules;
  no allocation.
