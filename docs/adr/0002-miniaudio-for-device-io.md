# 0002. miniaudio for audio device I/O

- Status: superseded by [0004](0004-native-audio-io.md) on 2026-11-12
- Date: 2026-11-11

## Context

We need low-latency output (and later input) on CoreAudio, WASAPI, ALSA,
PulseAudio and JACK, with as little build friction as possible.

## Decision

Use [miniaudio](https://miniaud.io) behind our own `vloom::AudioDevice`
wrapper. miniaudio is confined to `src/io/` and `src/render/`; its header never
appears in the public API.

## Alternatives considered

- **RtAudio** — mature, but needs per-platform build flags and system packages.
- **PortAudio** — mature, older C API, more involved to build on Windows.

## Consequences

- One header, no system packages on any platform, public domain / MIT-0 licence.
- Backend quirks are handled by miniaudio. If we outgrow it, only `src/io/`
  changes because the rest of the engine sees `vloom::AudioDevice`.
- ASIO on Windows is not included. Revisit when users ask for it.
