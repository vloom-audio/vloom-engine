# Changelog

All notable changes to vloom-engine are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Audio output on macOS, Windows and Linux via miniaudio (`io/AudioDevice`)
- Wait-free command queue from control thread to audio thread (`core/SpscQueue`)
- Sample-accurate, drift-free transport at 960 PPQ (`time/Transport`)
- Metronome (`dsp/Metronome`)
- Offline rendering to WAV (`render/OfflineRenderer`)
- `vloom-cli` with `devices`, `play` and `render` commands
