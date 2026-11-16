# 0013. MIDI channels on notes and controls, for MPE

- Status: accepted
- Date: 2026-11-17

## Context

An MPE controller (a Seaboard, a LinnStrument, an Osmose) bends and shapes
each note on its own. MIDI has pitch bend, pressure and controllers only
per channel, so MPE gives every sounding note a channel to itself: the bend
on channel 5 belongs to the one note that is on channel 5.

The engine's notes and controls (ADR 0012) had no channel. Everything went
to a plugin on channel 0, so a bend moved every note at once and an MPE
instrument played like an ordinary one.

## Decision

1. **`NoteEvent` and `ControlEvent` carry a MIDI channel**, 0 .. 15, 0 by
   default. `PluginInstance::playNote()`, `stopNote()` and `sendControl()`
   take it as a last argument. Code that knows no channels is unchanged.
2. **Each host passes the channel on in its format's own way**: the low
   half of the MIDI status for VST 2 (in the host and across the bridge)
   and for CLAP's MIDI events; the channel field of CLAP's and VST 3's note
   events. Notes a plugin puts out come back with their channel.
3. **VST 3 controls are looked up per channel.** VST 3 has no MIDI control
   events; a plugin says which parameter a control moves, and an MPE
   plugin says so for each channel. The host asks for all 16 when the
   plugin is created (the audio thread must not ask).
4. **Keys are tracked per channel** wherever the host ends notes itself (a
   reset, a note effect taken away, a source channel that stops): the same
   key can be down on two channels, and each has to end on its own.
5. **`ControlPoint` has a channel too**, so controls written into the song
   can address one. A pitch left bent when the song stops is centred on
   every channel it was bent on.
6. **The host does not act as the MPE zone manager.** An app that takes an
   MPE controller passes on what the controller sends, channel for
   channel, including the message that sets up the zone (it is three
   controller messages, sent with `sendControl()`).

## Alternatives considered

- **Per-note expression instead of channels** (CLAP note expressions, VST 3
  `INoteExpressionController`, a note id on every event) — closer to what
  the formats offer natively, and independent of MIDI's 16 channels. It
  needs a note id through the sequencer, the note effects and every host,
  and an app that turns a controller's channels into note ids. Channels
  are what controllers send and what MPE instruments accept in all three
  formats; ids can be added on top.
- **A channel on the plugin instance** (all its notes on channel n) — would
  not let two notes differ, which is the point of MPE.

## Consequences

- Notes written into the song (`Note`) still have no channel: they play on
  channel 0. MPE reaches a plugin live, and through control points.
- CLAP plugins that take notes as CLAP events only get the channel of
  their notes, but still no pitch bend or controllers (ADR 0012).
- A VST 3 plugin is asked 16 x 130 questions when it is created.
- Per-note expression in the formats' own terms is not sent.
