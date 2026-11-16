# 0014. Surround plugins in a stereo mixer

- Status: accepted
- Date: 2026-11-17

## Context

Some plugins work in a speaker layout wider than stereo: a 5.1 reverb, an
upmixer, an instrument with a surround output. The host asked every plugin
for stereo, and when a plugin kept its own layout it used the first two
channels of that and dropped the rest. A 5.1 plugin lost its centre and
its surrounds; a plugin that insists on its layout being confirmed did not
start at all.

The engine's mixer is stereo from the channels to the device (ADR 0007):
every bus, send, effect call, delay line, meter and export is two channels.

## Decision

1. **A plugin runs in the layout it has.** The host still asks for stereo
   first, so nothing changes for a plugin that can do stereo. A VST 3
   plugin that refuses is told that the host takes the arrangements it
   has, as the format wants.
2. **The host knows which speaker each channel is**: from the speaker
   arrangement in VST 3, from the surround extension in CLAP, or, where a
   CLAP plugin names no speakers, from the usual order for that many
   channels (5.1 is L R C LFE Ls Rs).
3. **In: the host's left and right go to the plugin's left and right
   speakers**, wherever those are in its order; its other inputs get
   silence. Spreading a stereo sound over more speakers is the plugin's
   work.
4. **Out: the plugin's speakers are folded down to stereo by the usual
   shares** (ITU-R BS.775): left and right as they are, the centre to both
   at -3 dB, a speaker on one side to that side at -3 dB, the bass channel
   left out. A speaker the host cannot place is left out too.
5. **A layout the host does not understand is used as before**: one channel
   as mono, the first two as left and right.
6. **The mixer stays stereo.** This is how surround plugins become usable
   in it, not surround mixing.

## Alternatives considered

- **A mixer as wide as the widest layout** — buses, sends, effects, delay
  compensation, meters, export and device output of n channels, and a
  panner for more than two speakers. That is the mixer written again, and
  the app's with it. It is the way to mix in surround; this decision does
  not start it and does not stand in its way (the layouts here are what it
  would need too).
- **Keeping the first two channels** — what was there: wrong for every
  plugin that puts sound anywhere but left and right.
- **Folding the bass channel in** — common in consumer devices, at -3 or
  -10 dB. Left out here, as in BS.775: it would add bass that the stereo
  mix already has in left and right.

## Consequences

- A 5.1 or 7.1 plugin is heard whole, in stereo. What it does with its
  surrounds is in the mix, at the fold-down's shares.
- The sum of several speakers can be louder than any one: a plugin that
  fills all of them comes out up to about 7.7 dB hotter than its left
  alone.
- VST 2 plugins are not covered: their extra channels are taken as more
  stereo outputs (ADR 0012), since the format rarely says otherwise.
- No surround panning, no surround buses, no multichannel export or
  device output.
