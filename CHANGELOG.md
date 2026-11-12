# Changelog

All notable changes to vloom-engine are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Audio output through each OS's own API, behind `io/AudioDevice`: WASAPI
  (shared, event driven, MMCSS "Pro Audio") on Windows, CoreAudio (AUHAL) on
  macOS, ALSA on Linux (also reaching PulseAudio and PipeWire)
- Own 32-bit float WAV writer (`render/WavWriter`)
- Wait-free command queue from control thread to audio thread (`core/SpscQueue`)
- Sample-accurate, drift-free transport at 960 PPQ (`time/Transport`)
- Metronome (`dsp/Metronome`)
- Master gain with a per-block ramp (`Engine::setMasterGain`)
- Smoothed CPU load of the audio callback (`Engine::cpuLoad`)
- Master balance with the same ramp as the gain (`Engine::setMasterPan`)
- Lock-free output peak metering for the UI (`Engine::takeOutputPeak`)
- Offline rendering to WAV (`render/OfflineRenderer`)
- `vloom-cli` with `devices`, `play` and `render` commands

### Removed
- miniaudio. It was used only for device output and WAV writing, but its
  whole source, decoders included, was built in; 0.11.21 carried
  CVE-2024-41147 (heap overflow in the FLAC decoder). See ADR 0004.
