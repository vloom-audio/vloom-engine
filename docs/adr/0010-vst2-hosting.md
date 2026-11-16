# 0010. Hosting VST 2 plugins without the VST 2 SDK

- Status: accepted
- Date: 2026-11-16

## Context

Many plugins people own exist only as VST 2: older instruments, plugins
whose maker never shipped a VST 3 or CLAP build. The engine hosts CLAP and
VST 3 (ADR 0006), so those plugins could not be used.

Steinberg stopped licensing the VST 2 SDK in 2018. A project that did not
sign the agreement before then cannot use its headers.

The interface itself is small: a module exports one function that returns a
structure of function pointers, and both sides call each other with numbered
requests. Other free hosts (Ardour, LMMS, Carla) host VST 2 from their own
description of that interface.

Other formats were asked for at the same time:

- AAX plugins load only in Pro Tools. They are signed for it, and Avid
  offers no way for another host to load them.
- Audio Units exist only on macOS and LV2 almost only on Linux. Neither can
  be run on the machine the engine is developed on.

## Decision

1. **The engine hosts VST 2 from its own description of the binary
   interface** (`src/plugin/Vst2Abi.hpp`): the structure, the requests and
   the event and song-position layouts the host uses. No Steinberg file is
   used.
2. **`Vst2Library` has the shape of `ClapLibrary` and `Vst3Library`**:
   `open(path)`, `plugins()`, `create(id)`. A VST 2 file holds one plugin,
   and only an instance can say what it is, so `open()` creates the plugin
   once and closes it again. The id is the plugin's four-character id as 8
   hex digits.
3. **What a VST 2 plugin gets**: 32-bit audio on its first two inputs and
   outputs, notes as MIDI with their frame in the block, parameter changes
   between blocks (VST 2 has no frame for them), the song position, tempo,
   time signature and loop, its state (a chunk, or its parameter values),
   its latency, and its editor on Windows and macOS.
4. **Not hosted**: shell files that hold many plugins, and VST 2 editors
   on Linux (they need the host's X11 display connection, and the engine
   has none). 32-bit plugins in a 64-bit host run through the bridge of
   ADR 0011.
5. **AAX is out of scope.** Audio Units and LV2 are left for when they can
   be tested on their own systems.

## Alternatives considered

- **The VST 2 SDK** — cannot be licensed any more.
- **A third-party header (VeSTige, FST)** — the same information under
  another licence and another project's naming. The part the host needs is
  about 150 lines, and writing it keeps the engine's dependencies as they
  are.
- **Only a bridge process per plugin** — keeps a crashing plugin away from
  the app, at the cost of two thread switches per block for every plugin.
  The bridge exists (ADR 0011) for the plugins that need it; the rest run
  in the host like CLAP and VST 3 plugins do.

## Consequences

- A VST 2 plugin runs in the app's process like the others, unless it is
  opened in the bridge: a plugin that crashes takes the app with it.
- Opening a VST 2 file starts the plugin. A scan of a folder of large
  instruments is slow, and on Windows every DLL in the folder is a
  candidate: an app should scan in a separate process and keep the result.
- Parameter changes reach a VST 2 plugin at the start of a block, up to one
  block late.
- The name "VST" is Steinberg's trademark. The engine may say that it loads
  VST 2 plugins; it may not carry the VST logo or call itself a VST product.
