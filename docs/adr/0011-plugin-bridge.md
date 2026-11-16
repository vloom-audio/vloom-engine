# 0011. A bridge process for plugins the host cannot load

- Status: accepted
- Date: 2026-11-16

## Context

A 64-bit process cannot load a 32-bit library. Many older VST 2 plugins
exist only for 32 bits, and with ADR 0010 the engine met them: it could say
what was wrong, but not use them.

The same question comes up for every plugin: it runs inside the host, so a
plugin that crashes takes the host with it (ADR 0006).

Both have one answer. The plugin runs in a small process of its own, and
that process and the host pass audio and requests to each other.

## Decision

1. **`vloom-plugin-bridge`** (`tools/vloom-plugin-bridge`) is a program that
   loads one VST 2 plugin. It is one source file that does not use the
   engine library, so it builds for another architecture than the engine:
   built for 32 bits it is `vloom-plugin-bridge32.exe`.
2. **Host and bridge share one block of memory**
   (`src/plugin/bridge/BridgeProtocol.hpp`), laid out the same for 32 and 64
   bits: an area for one control request and its answer, what the plugin
   is, a ring of things the plugin tells the host (edits in its editor, a
   size it wants), and one audio block: the sound in and out, a
   sidechain, eight more output pairs, the block's MIDI and the notes the
   plugin sends back. Four events say "a request is there" and "it is
   answered", for the control thread and for the audio thread. State too large for the block goes through a second block made
   for that one transfer.
3. **Nothing they share has a name.** The host makes the memory and the
   events, lets the bridge inherit exactly those handles (and one on the
   host process itself), and gives it their numbers on its command line. A
   block for state is passed by copying its handle between the two
   processes. No other process can open any of it.
4. **The audio thread waits for the bridge**, for twice the time the block
   lasts (at least 50 ms, at most 200 ms). If the
   bridge has ended, the plugin is given up for good: it passes audio
   through and `hasFailed()` says so. If the bridge is only slow (a sampler
   loading), the audio passes through until it answers, and then the plugin
   is back. The bridge's audio thread runs in the system's audio class, so
   the host does not wait on a thread slower than its own.
5. **The editor is the plugin's own window in the bridge process**, made a
   child of the window the host gives. A window handle means the same in
   both processes. Two things follow from the window living in another
   process:
   - making, moving and closing a child window sends messages to the
     parent and waits for the answer. The host's control thread owns the
     parent, so while it waits for the bridge it lets sent messages
     through;
   - a plugin may run a message loop of its own (a dialog, a menu). The
     bridge gets a control request as a message to a hidden window, so that
     loop delivers it and the request is answered all the same.
6. **A 32-bit plugin goes to the bridge by itself.** `Vst2Library::open()`
   reads the file's header; for a 32-bit one it looks for
   `vloom-plugin-bridge32.exe` next to the running program, or the file the
   `VLOOM_PLUGIN_BRIDGE32` environment variable names. Without it, opening
   fails and says so.
7. **Any VST 2 plugin can be bridged on request**
   (`Vst2Library::openBridged()`), to keep its crashes out of the host.
8. **Neither side trusts the other's numbers.** Each reads a count or size
   the other wrote once, and clamps it before use. A block of state is
   checked to be memory that is really there before it is copied.
9. **The bridge does not outlive the host.** It watches the host process on
   a thread of its own and ends at once when the host is gone, also while
   its main thread is stuck inside the plugin. The host also puts it in a
   job that ends it when the host's handle on it closes.
10. **A bridge that is gone saves nothing.** `saveState()` returns nothing,
    not an empty state, so that a host does not write it over the last good
    one.
11. **A bridge that ended is started again**, from the host's idle time, at
    most every two seconds and three times in all. The plugin gets the
    state that was last saved and is activated as it was. Its editor is
    not opened again: the host was told it closed. `hasFailed()` is true
    only once the three starts are used up.

## Alternatives considered

- **Sockets or pipes for the audio** — a copy and a system call per
  direction for every block. Shared memory needs the two signals only.
- **Spinning instead of waiting on an event** — lower latency on a busy
  machine, at the price of a core kept busy per plugin. The event costs two
  thread switches per block, which a bridged plugin can afford.
- **A bridge for VST 3 and CLAP too** — the same idea, but their interfaces
  are far larger (parameter queues, buses, message passing between two
  objects). It can be added behind the same protocol when it is asked for.
- **No 32-bit support** — what most new hosts do. The plugins asked for are
  ones that will never get a 64-bit build.

## Consequences

- An application that wants 32-bit plugins has to ship
  `vloom-plugin-bridge32.exe` next to itself, built with a 32-bit compiler.
- A bridged plugin costs two thread switches per block and one process.
  Of its outputs, the main pair and eight more cross the bridge.
- The audio thread waits on another process for every block of a bridged
  plugin, for up to twice the block's length when a bridge is slow to
  answer. This is the one place the engine's rule against waiting on
  the audio thread (ADR 0005) is set aside.
- The bridge keeps crashes out of the host. It is not a sandbox: the
  plugin's code runs with the user's rights there as it would in the host.
- A plugin started again has the state of the last save, not what was
  set since. Starting it blocks the control thread while it loads.
- The automated tests cover the requests, not a real window. Opening and
  closing a real 32-bit plugin's editor inside a window of the host was
  tried by hand.
- Windows only. macOS no longer runs 32-bit code at all, and 32-bit Linux
  plugins are rare.
