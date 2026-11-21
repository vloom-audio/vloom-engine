# 0020. WASAPI exclusive mode, with no fallback to shared

- Status: accepted
- Date: 2026-11-21

## Context

On Windows the output runs in WASAPI shared mode (ADR 0004): the engine
writes float32 into the Windows mixer, which resamples, converts channels,
mixes in other applications and applies its limiter to samples over 0 dBFS.
That is the safe default, but it is not what a user mastering or checking a
mix wants:

- The output is not bit-exact: the mixer may resample and always applies
  its own processing.
- Overs are compressed by the limiter instead of clipping, so a mix that
  clips sounds fine on the machine that made it.
- The mixer adds a period of latency.

WASAPI exclusive mode gives the stream the device directly. In exchange,
Windows converts nothing: the device must accept the rate, channel count
and sample format as they are, and no other application can play while the
stream is open.

## Decision

1. **Opt-in per open.** `AudioDeviceConfig::exclusive` (default false)
   asks for exclusive mode; `AudioDevice::isExclusive()` reports whether the
   open stream runs exclusive. Other platforms ignore the flag and report
   false. With the flag off, the shared path behaves exactly as before.
2. **No fallback.** If the device is in use (`AUDCLNT_E_DEVICE_IN_USE`),
   forbids exclusive use (`AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED`), takes
   none of our formats at the rate, or refuses the stream for another
   reason, `open()` returns false with a message. The caller decides
   whether to retry in shared mode and how to tell the user; a silent
   fallback would claim a bit-exact path that is not there.
3. **Formats tried, best first**, each as a `WAVEFORMATEXTENSIBLE` with the
   channel count's speaker mask, through
   `IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, ...)`:
   float32; int32 (32 valid bits); int24 in a 32-bit container (24 valid
   bits, most significant); packed int24 (3 bytes); int16. The first one
   the device takes is used. The rate is the requested one, or the mix
   format's when the request is 0. When none fits, the message names the
   rate and, if the device takes its mix format's rate instead, that rate.
4. **Event driven, buffer = period.** `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
   with `hnsBufferDuration == hnsPeriodicity`: the requested buffer size,
   at least the device's minimum period. On
   `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` a new client is initialised with the
   aligned size the failed one reports (Microsoft's recipe). Every event
   fills the whole buffer.
5. **Hard clip, never wrap.** The engine renders float into a scratch
   buffer allocated at open. `convertFromFloat()` (`src/io/SampleFormat`,
   pure and built on every platform so it is tested everywhere) writes it
   into the device format on the audio thread, without allocation: NaN
   becomes 0, samples are clamped to [-1, 1], scaled by 2^(bits-1) - 1
   (in double, so int32 stays exact) and rounded to nearest, halves away
   from zero. Full scale is symmetric: -1 maps to -32767, not -32768.
   Float32 is clamped the same way, so overs clip in every exclusive
   format; in-range float samples pass bit for bit.
6. **Same device-loss handling as shared:** a failed buffer call or a
   silent event stops the stream and `isRunning()` turns false.

## Alternatives considered

- Fall back to shared mode when exclusive fails — hides the failure from
  the user; the UI could not tell them their output is not bit-exact.
- Let the driver clip float32 overs — behaviour differs between drivers;
  some pass overs to a DAC that wraps or distorts.
- Dither when reducing to int16 or int24 — dither is a mastering choice
  that belongs in the mix (a plugin), not in the device layer, and it would
  add noise to a path chosen for being exact. int16 is the last format
  tried, for devices that take nothing better.
- Asymmetric scaling (-1 → -32768) — makes +1 and -1 unequal and clips +1
  by one code; the one lost code is inaudible.
- Timer-driven (polling) exclusive mode — needs larger buffers for the same
  safety; event-driven is Microsoft's recommended low-latency mode.

## Consequences

- Users can get a bit-exact, lower-latency output that clips audibly.
- While the stream is open, no other application can play on that device.
- The device must support the project's sample rate: a project at 44.1 kHz
  on a device that only takes 48 kHz fails to open in exclusive mode; the
  message names the rate so the UI can say why.
- Some drivers report formats they then refuse at `Initialize`; the open
  then fails with the HRESULT in the message rather than trying the next
  format.
- The live path is not covered by automated tests, which never open a
  device; it has to be checked on real hardware.
