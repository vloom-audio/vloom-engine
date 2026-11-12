# 0004. Native audio I/O instead of miniaudio

- Status: accepted
- Date: 2026-11-12
- Supersedes: [0002](0002-miniaudio-for-device-io.md)

## Context

ADR 0002 put miniaudio behind `vloom::AudioDevice`. We used only a small part
of it: opening an output device and writing WAV files (about 280 lines of
ours). Yet the whole library was compiled in, including its decoders, and the
pinned version (0.11.21) carried CVE-2024-41147, a heap overflow in its FLAC
decoder. M1 is about to read user sample files, which would have made such
decoder bugs reachable.

A DAW also has needs miniaudio does not cover, ASIO on Windows first. Owning
the device layer lets us add those, control thread priorities and latency,
and audit every line that runs on the audio thread.

## Decision

Talk to each operating system's audio API directly, behind the unchanged
`vloom::AudioDevice` API:

| OS | API | Notes |
|---|---|---|
| Windows | WASAPI, shared mode, event driven | COM on one MTA worker thread; MMCSS "Pro Audio"; Windows converts rate and channels (`AUTOCONVERTPCM`) |
| macOS | CoreAudio, HAL output unit (AUHAL) | The unit converts to the device format |
| Linux | ALSA | PulseAudio and PipeWire through ALSA's `default` / `pulse` / `pipewire` devices; `plughw:` for cards |

Each backend is one file in `src/io/` behind the internal
`io::AudioBackend` interface; only that file includes the platform headers.
WAV files are written by our own `render/WavWriter`.

## Alternatives considered

- **Keep miniaudio, upgrade to 0.11.26 and compile out its decoders**: least
  work and the CVE goes away, but we keep a large third-party C file on the
  audio thread and still cannot add ASIO there.
- **RtAudio / PortAudio**: same trade-off as miniaudio (see ADR 0002).

## Consequences

- No third-party library at run time; ALSA (`libasound2-dev`) is the only
  system package, on Linux.
- We maintain three backends. CI builds all three; only the Windows backend is
  tested on hardware so far. Tests that need a device are tagged `[hardware]`
  and hidden: run them with `vloom_engine_tests "[hardware]"` on a machine with
  speakers.
- A lost device (unplugged) behaves the same on every OS: the backend stops
  sending audio and `isRunning()` turns false. No backend moves to another
  output on its own (CoreAudio's output unit would; it is kept silent). What
  happens next is the host's policy: VloomDAW falls back to the system default
  and returns to the user's device when it is plugged in again; `vloom-cli
  play` stops with an error.
- Not covered yet: WASAPI exclusive mode and ASIO (Windows), JACK (Linux),
  device-change notifications (hosts poll for a returning device), audio
  input. Each can be added as another backend or option.
- Decoding sample files (M1) will be our own code too: WAV first, other
  formats through dedicated, fuzzed libraries when needed.
