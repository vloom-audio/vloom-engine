# Changelog

All notable changes to vloom-engine are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Security
- Supply chain: GitHub Actions are pinned to commit SHAs (checkout v7.0.1, cache
  v6.1.0; Dependabot keeps them current), the CPM.cmake download is checked against a
  pinned SHA-256 (a cached copy that does not match is fetched again), and Catch2 is
  pinned to the commit of its v3.7.1 tag.

### Added
- Audio output on macOS, Windows and Linux via miniaudio (`io/AudioDevice`)
- Wait-free command queue from control thread to audio thread (`core/SpscQueue`)
- Sample-accurate, drift-free transport at 960 PPQ (`time/Transport`)
- Metronome (`dsp/Metronome`)
- Master gain with a per-block ramp (`Engine::setMasterGain`)
- Smoothed CPU load of the audio callback (`Engine::cpuLoad`)
- Master balance with the same ramp as the gain (`Engine::setMasterPan`)
- Lock-free output peak metering for the UI (`Engine::takeOutputPeak`)
- Offline rendering to WAV (`render/OfflineRenderer`)
- `vloom-cli` with `devices`, `play` and `render` commands
