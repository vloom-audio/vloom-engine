# 0016. Audio input, monitoring and recording it

- Status: accepted
- Date: 2026-11-17

## Context

M6's other half: hearing and recording what comes in through a
microphone or an audio interface. ADR 0015 covered MIDI.

The engine only had output (ADR 0004): one `AudioDevice`, whose callback
renders the engine. The input may well be another device than the output
(a USB microphone, the laptop's speakers), with a clock of its own, and its
API differs on every OS as much as output's does.

What was recorded must land where the player heard the song when they
played along, not where the engine was when the input arrived. Between the
two lie the output's latency, the plugins' (ADR 0007), the input's latency,
and however long the input waited in the engine.

## Decision

1. **`AudioInput`, beside `AudioDevice`**, with a capture backend per OS in
   files of its own (`<Name>InputBackend.cpp`, behind `io::InputBackend`),
   so playback's backends are not touched: WASAPI shared mode, event
   driven (as output); CoreAudio's HAL unit with only its input enabled,
   pulled with `AudioUnitRender()`; ALSA capture with blocking reads on a
   thread of its own. The WASAPI helpers both directions use moved to
   `WasapiCommon.hpp`.
2. **The input reaches the engine through a lock-free ring**
   (`Engine::pushAudioInput()`, from the input's thread). The audio thread
   takes one block per block it renders. Too little waiting gives silence
   and counts a dropout (`inputDropouts()`). More than about four blocks
   waiting drops the oldest, so an input clock that runs fast cannot make
   the input lag more and more. The two clocks are not otherwise matched:
   no resampling.
3. **Monitoring** (`setInputMonitor(insert, gain)`) adds the input into an
   insert's bus, where it goes through the insert's effects, volume and
   routes like a channel's sound.
4. **Recording happens on the audio thread, writing on the control
   thread.** While `setAudioRecording(true)` is on and the song plays, every
   block's input goes into a second ring (about 10 s), with **marks**: where
   a take starts (the song's position, and the shift: the round trip set
   with `setAudioRecordLatency()`, plus the plugins' latency, plus the input
   waiting), and where the position jumps (a loop going back, a locate).
   `AudioTake` empties both into a WAV file as it goes (`WavFileWriter`)
   and works out **segments**: which stretch of the file plays where. A
   take's first frames are moved back by the shift, and a jump shows in the
   input that many frames after the engine made it. Each loop pass and each
   take is a segment of its own. Frames the reader did not take in time are
   lost whole blocks at a time; the next block written is marked where it
   is, so what follows stays in place.
5. **The engine does not put the recording into the song.** Segments are
   what audio clips (`SongAudioClip`) need; what a take replaces, and which
   pass is kept, is the app's choice.

## Consequences

- An input can be heard and recorded with its timing put right, on all
  three OSes, through the same engine calls.
- **The round trip comes from the app** (`setAudioRecordLatency()`): no
  backend reports its latency yet. Measuring it (a loopback test) is the
  usual way to get it right to the sample.
- **Clock drift** between two devices shows as a dropout or a dropped block
  now and then, not as a slow slip: there is no resampling.
- **Stereo only**: the engine takes a left and a right (or one channel on
  both); which inputs of a bigger interface those are is the app's choice
  in `AudioInput`'s callback.
- **Monitoring goes through the plugins' delay compensation** like the
  channels: a plugin with latency on the monitored insert makes the player
  hear themselves that much later.
- On macOS the device is asked to run at the engine's rate (the HAL unit
  converts none on input); another program using it then hears the change.
- The tests open no device: no microphone is touched, nothing is heard.
  The capture backends of macOS and Linux are only built by CI; none of
  the three has recorded from a real microphone yet.

## Alternatives considered

- **One duplex stream per device**: simpler timing where input and output
  are the same device, but useless where they are not, which is common.
  Separate streams work both ways.
- **Resampling the input to the output's clock**: the right answer for
  long takes on two devices; it needs the drift measured and a resampler
  on the audio thread. Left for later; the ring keeps it bounded meanwhile.
- **Writing the file on the audio thread**: never; disk writes block.
