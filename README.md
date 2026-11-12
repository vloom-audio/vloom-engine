# vloom-engine

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-brightgreen.svg)](#requirements)
[![Platforms](https://img.shields.io/badge/Platforms-macOS%20|%20Linux%20|%20Windows-lightgrey.svg)](#quick-start)
[![Status: M1](https://img.shields.io/badge/Status-M1%20(Channel%20Rack)-orange.svg)](#roadmap)

The real-time audio engine behind [VloomDAW](https://github.com/vloom-audio/VloomDAW).
A plain **C++20** library with no UI dependency, so it can be tested headless and
reused by other Vloom Audio tools.

> **Current milestone: M1 — channel rack.** Plays WAV samples from a step
> sequencer, sample-accurately alongside the metronome, and mixes each channel
> into one of up to 125 insert buses. Outputs through each OS's own audio API,
> renders offline to WAV, and ships the `vloom-cli` developer tool.

---

## Design rules

- **No UI dependency.** Builds, tests and runs without a display, so it works in
  CI, in `vloom-cli` and inside VloomDAW alike.
- **The audio thread never blocks.** Code reached from `vloom::Engine::process()`
  does not allocate, lock, wait, log, do I/O or throw.
  See [docs/architecture.md](docs/architecture.md).
- **Sample-accurate musical time.** The transport counts 960 ticks per quarter
  note and computes the position from an anchor point, so rounding errors do
  not add up over long sessions.
- **Lock-free control.** The UI or CLI sends commands to the audio thread
  through a single-producer / single-consumer queue (`core/SpscQueue`); state
  comes back through atomics.
- **Native audio I/O, no third-party audio library**: WASAPI on Windows,
  CoreAudio on macOS, ALSA on Linux (PulseAudio and PipeWire through ALSA).
  See [ADR 0004](docs/adr/0004-native-audio-io.md).
- **Offline rendering** to 32-bit float WAV, used by the tests and by
  `vloom-cli render`.
- **Channel rack (M1)**: a step sequencer that plays WAV samples (our own
  reader, built for untrusted files) with per-channel polyphony; edits are
  published as immutable snapshots, so they never block or cut the audio
  ([ADR 0005](docs/adr/0005-state-snapshots-for-the-audio-thread.md)).
- **Mixer routing (M1)**: each channel plays into an insert bus (or straight
  into the master) with its own volume and pan; inserts have volume, pan and a
  peak meter. Level changes ramp over one block, so they do not click.

---

## Requirements

- CMake ≥ 3.22 and Git
- A C++20 compiler: Apple Clang 15+ (macOS), GCC 12+ (Linux), MSVC 2022 (Windows)
- Linux only: the ALSA headers (`libasound2-dev` on Debian/Ubuntu, `alsa-lib-devel` on Fedora)
- The test framework (`Catch2`) is downloaded and pinned by CMake through
  [CPM](https://github.com/cpm-cmake/CPM.cmake); the engine itself has no
  third-party runtime dependency (`cmake/Dependencies.cmake`).

---

## Quick start

### 1. Build and test

```sh
git clone https://github.com/vloom-audio/vloom-engine.git
cd vloom-engine

cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

### CMake presets

| Preset | Use it for |
|---|---|
| `dev` | Day-to-day work: Debug build, compiler warnings are errors |
| `release` | Optimised build |
| `asan` | Debug + AddressSanitizer / UndefinedBehaviorSanitizer (memory bugs) |
| `tsan` | Debug + ThreadSanitizer (data races) |

### 2. Run the CLI

`vloom-cli` is built to `build/<preset>/tools/vloom-cli/`:

```sh
# List audio output devices
./build/dev/tools/vloom-cli/vloom-cli devices

# Play the metronome at 128 BPM on the default device
./build/dev/tools/vloom-cli/vloom-cli play --bpm 128

# Render 8 seconds of metronome at 140 BPM to a WAV file
./build/dev/tools/vloom-cli/vloom-cli render click.wav --bpm 140 --seconds 8

# Play a channel rack: one --channel per sample, x = step on (16th notes)
./build/dev/tools/vloom-cli/vloom-cli play --bpm 128 \
    --channel kick.wav:x...x...x...x... \
    --channel hat.wav:..x...x...x...x.
```

Platform setup and troubleshooting: **[docs/getting-started.md](docs/getting-started.md)**.

---

## Repository layout

```
vloom-engine/
├── include/vloom/engine/     # Public API (what VloomDAW may include)
│   ├── Engine.hpp            # The engine: process(), play / stop / tempo control
│   ├── core/                 # SpscQueue, SnapshotExchange, version info
│   ├── dsp/                  # Sound generators and processors (Metronome, Sampler)
│   ├── io/                   # Hardware audio devices (WASAPI, CoreAudio, ALSA)
│   ├── mixer/                # Insert settings and the pan law
│   ├── render/               # Offline rendering, WAV writer
│   ├── sample/               # WAV reader, resampler, sample loading
│   ├── sequencer/            # Channel rack state and step playback
│   └── time/                 # Musical time, Transport
├── src/                      # Implementation, same module folders as include/
├── tests/                    # Catch2 tests, same module folders as src/
│   └── support/              # Test helpers (audio assertions)
├── tools/vloom-cli/          # Command-line tool (devices / play / render)
├── cmake/                    # Build helpers; all dependencies in Dependencies.cmake
├── docs/                     # Guides, architecture and decision records
│   ├── adr/                  # Architecture Decision Records
│   ├── architecture.md       # Modules, threads, control flow, time
│   ├── coding-guidelines.md  # C++ conventions and real-time rules
│   └── getting-started.md    # Machine setup (macOS, Linux, Windows)
└── scripts/                  # Developer scripts (format.sh)
```

---

## Roadmap

| Milestone | Deliverables | Status |
|:---:|---|:---:|
| **M0** | Audio device output, transport, metronome, offline WAV render, CLI | *Done* |
| **M1** | Step sequencer and sample playback (drum channel rack); mixer routing to inserts | *Done* |
| **M1.5** | Sample preview voice | *Next* |
| **M2** | MIDI notes, pitched sampler, first built-in synth, master pitch | *Planned* |
| **M3** | Arrangement (patterns on a timeline), song mode, WAV export | *Planned* |
| **M4** | Plugin host interface, CLAP hosting | *Planned* |
| **M5** | VST3 hosting, built-in effects, sends, delay compensation | *Planned* |
| **M6** | Audio input and recording, MIDI input | *Planned* |

Details: [docs/architecture.md#roadmap](docs/architecture.md#roadmap); the
whole plan, engine and app, is in VloomDAW's
[docs/roadmap.md](https://github.com/vloom-audio/VloomDAW/blob/main/docs/roadmap.md).

---

## Documentation

- **[Getting started](docs/getting-started.md)**: tools to install, building, IDE setup.
- **[Architecture](docs/architecture.md)**: modules, threads, how commands reach the audio thread.
- **[Coding guidelines](docs/coding-guidelines.md)**: naming, formatting, real-time rules.
- **[Decision records](docs/adr/)**: why things are the way they are.

---

## Contributing

Contributions and bug reports are welcome. Before opening a pull request:

1. Run the tests: `ctest --preset dev`.
2. Format your code: `scripts/format.sh` (needs clang-format 23.1.3; see getting-started).
3. Keep the audio thread real-time safe: no locks or allocations in `process()`.
4. Read the organisation's [contributing guide](https://github.com/vloom-audio/.github/blob/main/CONTRIBUTING.md).

---

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
