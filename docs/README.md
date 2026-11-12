# vloom-engine documentation

- [Getting started](getting-started.md) — tools, build, run, day-to-day commands
- [Architecture](architecture.md) — modules, threads, time, roadmap
- [Coding guidelines](coding-guidelines.md) — real-time rules, where code goes, style
- [Decision records](adr/) — why things are the way they are
  - [0001 Own engine without JUCE](adr/0001-own-engine-without-juce.md)
  - [0002 miniaudio for device I/O](adr/0002-miniaudio-for-device-io.md) (superseded by 0004)
  - [0003 GPL-3.0-or-later](adr/0003-gpl-3-or-later.md)
  - [0004 Native audio I/O instead of miniaudio](adr/0004-native-audio-io.md)
  - [0005 Immutable snapshots for state the audio thread reads](adr/0005-state-snapshots-for-the-audio-thread.md)
