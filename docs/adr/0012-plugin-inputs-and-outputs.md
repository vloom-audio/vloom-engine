# 0012. More than a plugin's main sound: sidechains, extra outputs, notes out, controls, programs

- Status: accepted
- Date: 2026-11-17

## Context

A hosted processor got one stereo sound in and gave one back (ADR 0006).
Whole kinds of plugins need more than that and loaded without being of
use:

- a compressor that ducks to a kick listens to a second input, its
  sidechain;
- a drum machine or sampler has an output per pad or group, to mix apart;
- an arpeggiator or chord maker puts out notes, not sound;
- an instrument is played with a pitch wheel, a mod wheel and pedals;
- many plugins list their programs for the host to choose from.

The engine's mixer is stereo: inserts with an output, sends and a graph
that orders them and lines up their delays (ADR 0007).

## Decision

1. **`ProcessContext` carries what is optional**: `sidechain` (two channels,
   or null), `extraOutputs` (buffers in pairs for the outputs after the main
   one), `notesOut` with its count, and `controls` (pitch bend, controller,
   pressure, each at a frame). A processor that knows none of them needs no
   change. `AudioProcessor` says what it has: `hasSidechain()`,
   `extraOutputCount()`, `makesNotes()`, `programs()`.
2. **A sidechain is a send.** `Send::sidechain` sends the copy to the target
   insert's sidechain instead of into its sound. It is a route like any
   other: ordered, checked for loops, lined up with the rest. All sidechain
   sends to an insert are added up, and every effect of the insert that has
   a second input hears the same.
3. **Extra outputs go to inserts.** `ChannelState::pluginOutputs[k]` names
   the insert for the instrument's output k + 2. They get the channel's
   volume and pan. An output that is not named is not played.
4. **A note effect sits in front of the instrument.**
   `ChannelState::noteEffect` gets the channel's notes; the instrument plays
   what it puts out, in the same block.
5. **Controls are sent to an instance** (`PluginInstance::sendControl()`),
   as notes played by hand are. Each format gets them its own way: MIDI for
   VST 2 and for CLAP plugins that take MIDI; for VST 3 the parameter the
   plugin says the control moves (`IMidiMapping`), asked once when the
   plugin is created, since the audio thread must not ask.
6. **Programs are a list of names and an index.** VST 2 has them as such;
   for VST 3 they are the first program list and the parameter marked as
   the program change.
7. **Stereo stays.** A sidechain and every extra output are two channels; a
   mono bus is mixed down or doubled. Surround would mean a mixer of
   another width from the channels to the device, not a change to plugin
   hosting.

## Alternatives considered

- **A sidechain input per effect slot**, each with its own source — more
  exact, and a second routing table next to the mixer's. One sidechain per
  insert covers the uses asked for and reuses the graph.
- **Notes between channels** (one channel's note effect driving another's
  instrument) — what a modular host does. It needs an order between
  channels the sequencer does not have. The slot in front of the instrument
  covers arpeggiators and chord makers.
- **Controls in the song** (automation lanes for pitch bend and
  controllers) — the sequencer's data has no place for them yet. This
  decision gives the host objects the path; the lanes can use it.

## Consequences

- An app has to offer the routing: a sidechain switch on a send, a target
  per extra output, a note-effect slot, a program list. Until it does, the
  engine behaves as before.
- A plugin's sidechain bus and its extra outputs are switched on when it is
  activated, whether routed or not: a plugin with many outputs computes
  them all.
- Extra outputs follow the channel's volume without a ramp: a jump in the
  volume can click there.
- CLAP plugins that take notes as CLAP events only get no pitch bend or
  controllers. CLAP has no list of programs to read.
- A bridged VST 2 plugin (ADR 0011) gets controls, but no sidechain and no
  extra outputs: only its first two inputs and outputs cross the bridge.
- VST 2 editors on Linux are still not hosted.
