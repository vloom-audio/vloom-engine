# 0015. MIDI input, live playing and recording it

- Status: accepted
- Date: 2026-11-17

## Context

M6 is audio input and recording, and MIDI input. This record covers the
MIDI half; audio input follows in its own record.

Before this, the only way to play a note that was not in the song was
`PluginInstance::playNote()`: for an instrument plugin only, from one
control thread, with nothing to say where in the song it was played. The
built-in synth and the sampler could not be played live at all, and
nothing could be recorded.

Three things are needed:

1. **Reading a keyboard.** Each OS has its own MIDI API, with its own
   thread and its own way of handing over messages.
2. **Playing what it sends** on any channel of the rack, with the
   channel's instrument, whatever the song is doing.
3. **Recording it**: where in the song each message was *heard*, put
   together into notes and controls that can be written into a pattern.

## Decision

1. **`MidiInput`, one per port**, with a backend per OS: WinMM on Windows
   (every version has it, every class-compliant keyboard shows up),
   CoreMIDI on macOS (a MIDI 1.0 port of the macOS 11 API; the older one
   is deprecated), the ALSA sequencer on Linux (USB keyboards and other
   programs' ports alike, PipeWire and JACK included). Each hands over
   short messages (`MidiMessage`) to a callback on its own thread. System
   exclusive and real-time bytes are not passed on. `stop()` and `close()`
   return only once the callback is no longer running.
2. **`Engine::playMidi(channel, message)` from any thread.** The callers
   take a short lock among themselves (several keyboards, an on-screen
   one); the audio thread does not take it. It reads a lock-free queue at
   the start of each block and plays the messages
   (`StepSequencer::playLive()`):
   - **Synth and sampler**: a note is held until its note-off. Each live
     key gets a mark in the voice's `releaseAt` from `kLiveFirst` (2^62)
     on, one per key and MIDI channel, which `releaseExactly()` ends. The
     song's releases never reach that far, so a loop, a jump or a stop of
     the song leaves the note alone: the key is still down.
   - **Instrument plugin**: the notes go in at the block's first frame
     with their MIDI channel (ADR 0013), through the channel's note
     effects; pitch bend, controllers and pressure go with the song's
     controls, before them.
   - A muted channel plays them too: the player hears what they play.
   - **All notes off** (controller 123, and all sound off, 120) ends every
     live note on its MIDI channel, whatever the instrument; an instrument
     plugin also gets a note-off for each key it holds there.
     `Engine::releaseLiveNotes()` sends it on every MIDI channel to every
     channel of the rack, and a `MidiInput` that is stopped or closed
     passes it to its callback last, so no key is left sounding.
3. **Recording happens on the audio thread, putting the take together
   on the control thread.** While `setRecording(true)` is on and the song
   plays, each live message goes into a second queue with the song
   position the player heard it at. That is the engine's position, less the
   plugins' latency (`latencyFrames()`) and the device's
   (`setRecordLatency()`). In a loop, the loop's pass goes with it, and a
   message heard just before the loop jumped back counts in the pass
   before. `takeRecordedMidi()` empties the queue; `MidiTake` turns the
   messages into notes (a key from going down to coming up) and controls.
   `quantize()`, `patternNotes()` and `patternControls()` make them ready
   for a pattern. The engine never writes into the song itself: what a
   take replaces or merges with is the app's choice.

## Consequences

- Any channel can be played from a keyboard, and what is played can be
  recorded with the latency taken off.
- **A live message waits for the next block**: up to one block late
  (5.3 ms at 256 frames, 48 kHz), and it is recorded at the block's
  start. Placing each message within the block from its timestamp would
  need the MIDI clock and the audio clock lined up; that is left for later.
- **The latency the player hears has to come from the app**
  (`setRecordLatency()`): `AudioDevice` does not report its output
  latency yet.
- **A pattern's notes have no MIDI channel.** A take keeps each note's
  channel (`RecordedNote::midiChannel`), but written into a pattern an MPE
  performance plays back on channel 0. Its controls keep theirs.
- WinMM gives a port to one program at a time: a keyboard another program
  holds cannot be opened. Windows MIDI Services, which shares ports, is
  not on every Windows yet.
- The tests use no MIDI hardware and open no port. The backends of macOS
  and Linux are only built by CI; none of the three has been tried with a
  real keyboard.

## Alternatives considered

- **RtMidi or PortMidi** instead of three backends: a dependency for
  about 300 lines, and neither does more than what is needed here.
- **Live notes through `PluginInstance::playNote()`**: for plugins only,
  and its queue has a single producer, so a MIDI thread and the UI could
  not both play.
- **Writing the take into the `SequencerState` from the engine**: the
  state belongs to the app (ADR 0005); the engine would have to know about
  undo, merging and which pattern is being recorded into.
