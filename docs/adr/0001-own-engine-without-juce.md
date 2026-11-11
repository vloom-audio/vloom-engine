# 0001. Write our own engine, without JUCE

- Status: accepted
- Date: 2026-11-11

## Context

VloomDAW needs an audio engine that runs on macOS, Windows and Linux, and a
workflow modelled on FL Studio (channel rack, patterns, playlist). The project
is 100% open source and funded by donations.

## Decision

Write the engine ourselves in C++20, in its own repository with no UI
dependency, using small focused libraries (miniaudio for device I/O) instead
of a framework.

## Alternatives considered

- **JUCE** — fastest path to a working DAW, but ties the whole project to
  AGPLv3 and to JUCE's architecture; CLAP hosting is limited.
- **Tracktion Engine** — a complete DAW engine, but its track/clip model does
  not fit a pattern-based workflow well.

## Consequences

- Full control over architecture, threading and licence (GPL-3.0-or-later).
- More work up front: device handling, plugin hosting and plugin windows must
  be written by us.
- The engine is testable headless and reusable by future Vloom Audio tools.
