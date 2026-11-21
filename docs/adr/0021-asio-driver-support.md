# 0021. ASIO driver support, one shared session for output and input

- Status: accepted
- Date: 2026-11-21

## Context

On Windows the engine talks to WASAPI (ADR 0004), shared or exclusive
(ADR 0020). Most audio interfaces sold for music also ship an ASIO driver,
and many users expect it: it is often the lowest-latency path the vendor
supports, it exposes all of the interface's channels, and it is the only
way to reach the vendor's own buffer and clock settings (its control
panel). WASAPI exclusive gets close on some devices, but not on all, and
it cannot open the driver's panel.

Facts that shape the design:

- **One driver per process.** The Steinberg SDK keeps the loaded driver in
  globals (`theAsioDriver`, `asioDrivers`), and its callbacks
  (`bufferSwitch`, `asioMessage`, ...) carry no user pointer. Whatever the
  API looks like, the process has one driver and one set of callbacks.
- **Input and output are one stream.** A driver delivers its inputs and
  takes its outputs in the same `bufferSwitch`, on one clock. The engine,
  though, has a separate `AudioDevice` (output) and `AudioInput` (ADR 0016),
  each with its own backend and thread.
- **Drivers are COM objects** that mostly expect a single-threaded
  apartment, and the thread that loaded one should also start, stop and
  unload it.
- **Licence.** The ASIO SDK 2.3.4 (2025-10-15) is licensed "under the terms
  of the Steinberg ASIO License, or alternatively under the terms of the
  General Public License (GPL) Version 3". That is GPLv3 only, not "or
  later". The host helpers (`host/asiodrivers.cpp`, `host/pc/asiolist.cpp`)
  also carry a BSD-style notice that redistributions must keep. The "ASIO"
  name and logo are trademarks: optional under GPLv3, and if used, Steinberg's
  usage guidelines apply.

## Decision

1. **An API choice, not a new class.** `enum class AudioApi { Default, Asio }`
   in `AudioDeviceConfig::api` and `AudioInputConfig::api`;
   `listOutputDevices(AudioApi)` and `listInputDevices(AudioApi)` list the
   installed ASIO drivers. `AudioDevice::isApiAvailable(AudioApi)` says
   whether the build and platform have it. The defaults (`AudioApi::Default`,
   `inputChannels = 0`) leave every existing path exactly as it was.
   `exclusive` is ignored for ASIO and `isExclusive()` reports false.
2. **One session per process** (`io::AsioSession`), held by `shared_ptr`
   by whoever uses it and destroyed when the last holder lets go. It owns
   the loaded driver and a driver thread of its own: COM is initialised
   there as a single-threaded apartment, every driver call (list, load,
   buffers, start, stop, control panel, unload) is made there, and the
   thread pumps window messages while it waits. Control calls from the
   API block until the driver thread has done them. A second `AudioDevice`
   opening ASIO while one is open fails with "already open in this process".
3. **Full duplex in this pass: the input taps the output's session.** The
   output opens the driver with `AudioDeviceConfig::inputChannels` inputs
   (from the first). An `AudioInput` with `AudioApi::Asio` does not load
   anything: it attaches a capture callback to the open session, and fails
   with a message when no ASIO output is open, when it was opened without
   inputs, when the index names another driver, or when it asks for another
   rate. In each `bufferSwitch` the input half is converted and handed to
   the input first, then the output is rendered: one thread, one clock, no
   drift for `Engine::pushAudioInput()` to absorb. When the output closes,
   the input stops running and must be opened again after the output.
4. **The driver behind an interface.** `io::AsioDriver` mirrors the ASIO
   calls the engine needs with plain types and the ASIO numeric constants
   (`src/io/AsioDriver.hpp`, no SDK include). `AsioSdkDriver.cpp` implements
   it with the SDK, on Windows builds with `VLOOM_ENGINE_ASIO` only. The
   session and both backends are plain C++ built on every platform and
   tested with a fake driver; no test loads a real driver or opens a device.
5. **Opening.**
   - Rate: `sampleRate = 0` keeps the driver's current rate; otherwise
     `ASIOCanSampleRate` then `ASIOSetSampleRate` when the current one
     differs. An unsupported rate fails `open()` with the driver's name, the
     rate and the ASIO error (`ASE_NoClock`).
   - Buffer: `bufferFrames` if `ASIOGetBufferSize` allows it (from `min` by
     `granularity`, `-1` = powers of two, `min == max` = that size only),
     else the driver's preferred size. `AudioDevice::bufferFrames()` reports
     it. A buffer larger than `kMaxBlockFrames` is rendered in chunks, as
     for every backend.
   - Channels: the first `channels` outputs and `inputChannels` inputs, up
     to what the driver has (and 8).
   - Latency: `ASIOGetLatencies` after the buffers exist, reported by
     `AudioDevice::inputLatencyFrames()` and `outputLatencyFrames()`; their
     sum is what `Engine::setAudioRecordLatency()` wants. Other APIs report
     0 (unknown).
   - `ASIOOutputReady()` is probed once at open and, if the driver takes it,
     called after every output fill.
   - Error texts name the driver and the ASIO error (`ASE_...`), and include
     the driver's own message when `ASIOInit` gives one.
6. **Formats.** Int16LSB, Int24LSB (packed), Int32LSB, Float32LSB,
   Float64LSB and Int32LSB16/18/20/24 (right-justified in 32 bits,
   sign-extended), per channel, through `convertFromFloat()` and the new
   `convertToFloat()` in `src/io/SampleFormat` (pure, tested on every
   platform). Output follows ADR 0020: clamp to [-1, 1], NaN to 0, scale by
   2^(bits-1) - 1, round to nearest. Input divides by the same scale, so
   codes of up to 24 bits round-trip; float input passes through (NaN as 0),
   overs included. Big-endian (MSB) and DSD types fail `open()` with the
   type's name. Both output halves are cleared right after
   `ASIOCreateBuffers`.
7. **Driver messages never act from the callback.** `asioMessage` answers
   `kAsioSelectorSupported` (for the selectors below), `kAsioEngineVersion`
   (2) and `kAsioSupportsTimeInfo` (yes, so drivers call
   `bufferSwitchTimeInfo`). `kAsioResetRequest`, `kAsioBufferSizeChange`,
   `kAsioResyncRequest` and `kAsioLatenciesChanged`, and a
   `sampleRateDidChange` to another rate, only set a flag:
   `AudioDevice::resetRequested()` turns true and the control thread closes
   and opens the device again.
8. **Control panel.** `AudioDevice::showControlPanel()` calls
   `ASIOControlPanel()` on the driver thread for an open ASIO stream; other
   APIs return false with a message.
9. **Real-time rules hold.** `bufferSwitch` does not allocate or lock: the
   scratch buffers are made at open, and the input tap and the "running"
   state are atomics. `stop()` and detaching the input clear their flag and
   wait until no callback is inside, so nothing is called after they
   return.
10. **Build and licence.** CMake option `VLOOM_ENGINE_ASIO` (default ON on
    Windows, ignored elsewhere). The SDK is never in the repository: it is
    downloaded from Steinberg at configure time, pinned by SHA-256, or taken
    from `VLOOM_ENGINE_ASIO_SDK_DIR` (an extracted `ASIOSDK` folder). Only
    `common/asio.cpp`, `host/asiodrivers.cpp` and `host/pc/asiolist.cpp`
    are compiled, into the static library `vloom_asio_sdk`, without our
    warnings and with SYSTEM include directories. The engine's source stays
    GPL-3.0-or-later; a binary built with ASIO uses the SDK under GPLv3 and
    is therefore distributed under GPL-3.0-only. Distributions keep the BSD
    notice of the host helpers. "ASIO" appears only as a driver-type label
    (`AudioApi::Asio`, messages), never in a product or library name of
    ours, and no logo is used.
11. **Non-Windows.** Everything SDK-related compiles out. The API stays:
    ASIO lists nothing and `open()` fails with "ASIO is not available on
    this platform" (on Windows with the option off: "ASIO support is not
    built in").

## Alternatives considered

- **Output first, input later.** Smaller, but the input is what ASIO users
  need for recording, and the session design made the tap small. An API
  shaped for output only would have needed breaking later.
- **A combined duplex device class.** Clean for ASIO, but it would split
  the engine's I/O model in two; `AudioInput` with `AudioApi::Asio` keeps
  one model for every API.
- **The input reopens the driver with more inputs.** Hides the coupling,
  but a glitch on the output whenever an input opens, and two objects
  fighting over one driver's settings. Declaring the inputs on the output
  is explicit.
- **Loading the driver on the caller's thread.** The caller's apartment is
  unknown (WASAPI joins the multi-threaded one, Qt's UI thread is
  single-threaded), and drivers misbehave when called from several threads.
- **Calling `IASIO` directly instead of the SDK host helpers.** Avoids the
  SDK's globals, but reimplements its driver list and load logic; the
  globals are confined to one file and one thread instead.
- **A licensed (non-GPL) SDK build.** The Steinberg ASIO License requires
  an agreement with Steinberg; not needed for a GPL project.

## Consequences

- Users with an audio interface get its ASIO driver, its full channel
  count, its control panel and a recording input on the output's clock.
- Windows builds with ASIO are GPL-3.0-only binaries. Builds that need
  "or later" (or prefer no download) set `VLOOM_ENGINE_ASIO=OFF`.
- Configuring a Windows build downloads the SDK once (cached by CPM), unless
  `VLOOM_ENGINE_ASIO_SDK_DIR` points at a local copy.
- Driver names are limited by the SDK to 31 characters (longer ones end in
  "..."); two drivers that differ only after that cannot both be chosen.
- Choosing which driver channels to open (other than "from the first") is
  not in this pass: a follow-up can add a first-channel offset to both
  configs without breaking them.
- The live path (a real driver's threads, formats, timing and control
  panel) is not covered by automated tests, which never load a driver: it
  has to be checked on real hardware.
