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
   insert's sidechain instead of into its sound. A VST 3 plugin's second
   input counts as one only if the plugin calls that bus auxiliary; it is
   then switched on whether anything is sent to it or not. It is a route like any
   other: ordered, checked for loops, lined up with the rest. All sidechain
   sends to an insert are added up, and every effect of the insert that has
   a second input hears the same, each as late as the effects before it
   make the sound it works on (their latency).
3. **Extra outputs go to inserts.** `ChannelState::pluginOutputs[k]` names
   the insert for the instrument's output k + 2. They get the channel's
   volume and pan, ramped over the block like its main sound. An output
   that is not named is not played.
4. **A note effect sits in front of the instrument.**
   `ChannelState::noteEffect` gets the channel's notes; the instrument plays
   what it puts out, in the same block, put in frame order by the host.
   `ChannelState::noteEffects` are more of them in a row, each fed by the
   one before. Notes played by hand on the instrument go through them too.
   - An effect that is bypassed, not active, or makes no notes is skipped:
     the channel's notes go to the instrument as if it were not there.
   - The host keeps track of the keys the effect has the instrument hold.
     It ends them itself when the effect is taken away, exchanged, switched
     off or on, and when the song stops or jumps (the effect is reset
     then): the effect's own note-offs can no longer be counted on.
   **A channel can play another channel's notes.**
   `ChannelState::notesFrom` names a channel whose notes (what its
   instrument is given, after its note effects) this channel's instrument
   plays too: one arpeggiator driving two instruments, a melody doubled on
   another sound. The source is processed first, whatever the order of the
   channels; a chain that comes back to itself is cut. Keys held from a
   source that is taken away are ended by the host.
5. **Controls are sent to an instance** (`PluginInstance::sendControl()`),
   as notes played by hand are. Each format gets them its own way: MIDI for
   VST 2 and for CLAP plugins that take MIDI; for VST 3 the parameter the
   plugin says the control moves (`IMidiMapping`), asked once when the
   plugin is created, since the audio thread must not ask.
   **Controls can be written into the song**: `ChannelState::controls` and
   `SongPattern::controls` are points with a tick, like notes. They go
   through the scheduler that starts notes, so each lands on its frame, in
   pattern mode and in a song's clips, and comes round when a pattern
   repeats. A pitch left bent when the song stops is put back to the
   centre.
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
- A send that would close a loop is left out, a sidechain send as much as
  any other. `droppedRoutes()` tells an app which. A sidechain send counts
  for the mixer's delay compensation even when no effect listens to it.
- A VST 2 plugin's inputs 3 and 4 are taken for the sidechain without the
  plugin saying so: the format has no way to say it. A CLAP plugin's
  second input port counts as one unless it is marked as a main port.
- MPE is ADR 0013. Surround is not done: a piece of work of its own.
- VST 3 controls reach only plugins that say which parameter a control
  moves, asked once for the first event bus and channel.
- CLAP plugins that take notes as CLAP events only get no pitch bend or
  controllers. CLAP has no list of programs to read.
- A bridged VST 2 plugin (ADR 0011) gets all of it as well; of its extra
  outputs the first eight pairs cross the bridge.
- VST 2 editors on Linux are still not hosted.
