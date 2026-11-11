# Coding guidelines

## 1. The audio thread is sacred

Code reachable from `Engine::process()` (and anything marked "audio thread")
must **never**:

- allocate or free (`new`, `delete`, `std::vector::push_back`, `std::string`, `std::function` copies …)
- lock a mutex, wait, sleep, or call into the OS scheduler
- log, print, or touch files / network
- throw exceptions

Mark such functions `noexcept`. Do allocation in `prepare()` or on the control
thread, then hand ready objects over. When in doubt, ask in your pull request.

## 2. Where does my code go?

| You are adding… | Header | Source | Test |
|---|---|---|---|
| A synth, effect or sound source | `include/vloom/engine/dsp/` | `src/dsp/` | `tests/dsp/` |
| Anything about tempo, bars, positions | `…/time/` | `src/time/` | `tests/time/` |
| Device / driver code | `…/io/` | `src/io/` | — (hardware) |
| A generic utility (queue, math, …) | `…/core/` | `src/core/` | `tests/core/` |
| A new export format | `…/render/` | `src/render/` | `tests/render/` |

Then:

1. Add the `.cpp` to its module block in `src/CMakeLists.txt`.
2. Add the test to `tests/CMakeLists.txt`.
3. If it is public API, include it from `include/vloom/engine.hpp`.

A new top-level module (for example `sequencer/`) needs a row in the module
table of [architecture.md](architecture.md).

## 3. Naming and style

- Formatting is automatic: run `scripts/format.sh` (`.clang-format`).
- Everything lives in `namespace vloom` (tools use `vloom::cli`, tests `vloom::test`).
- Types `PascalCase`, functions and variables `camelCase`, members end in `_`,
  constants `kPascalCase`.
- Files are named after the main type: `Transport.hpp` / `Transport.cpp` / `TransportTests.cpp`.
- Every file starts with the SPDX licence line and copyright line.
- Public headers use `#pragma once` and include what they use.
- Prefer `[[nodiscard]]` on getters and `noexcept` where it is true.

## 4. Tests

- Every bug fix comes with a test that fails without the fix.
- DSP tests render offline and check exact samples or onsets. Never sleep or
  depend on timing.
- Tag tests with their module: `TEST_CASE("…", "[time]")`.

## 5. Dependencies

New third-party libraries need an issue first. When agreed, add them to
`cmake/Dependencies.cmake` (tests only: `tests/CMakeLists.txt`) with a pinned
version and a one-line comment saying why. Licences must be GPL-3.0-compatible.

## 6. Decisions

A choice that future contributors will ask "why?" about (a library, a threading
model, a file format) gets an Architecture Decision Record: copy
[`adr/0000-template.md`](adr/0000-template.md) to the next number.
