# Getting started

## 1. Install the tools

| OS | Command |
|---|---|
| macOS | `xcode-select --install` then `brew install cmake ninja pipx` |
| Windows | Visual Studio 2022 (workload *Desktop development with C++*) and CMake |
| Ubuntu / Debian | `sudo apt install build-essential cmake ninja-build pipx libasound2-dev` |

Formatting uses **clang-format 23.1.3 exactly** (other versions format differently and
fail CI). Install it on any OS with `pipx install clang-format==23.1.3`
(or `pip install --user clang-format==23.1.3`). If another clang-format comes first on
your `PATH`, point the script at the right one: `export CLANG_FORMAT=/path/to/clang-format`.

## 2. Get the code

Clone the engine next to the app so you can work on both:

```sh
mkdir vloom-audio && cd vloom-audio
git clone https://github.com/vloom-audio/vloom-engine.git
git clone https://github.com/vloom-audio/VloomDAW.git
```

## 3. Build and test

```sh
cd vloom-engine
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The first configure downloads dependencies. To share them between builds and
repos, set once in your shell profile:

```sh
export CPM_SOURCE_CACHE="$HOME/.cache/cpm"
```

## 4. Hear it

```sh
./build/dev/tools/vloom-cli/vloom-cli devices          # list outputs
./build/dev/tools/vloom-cli/vloom-cli play --bpm 128   # metronome for 8 s
./build/dev/tools/vloom-cli/vloom-cli render out.wav   # 4 s offline render
```

## 5. Editor setup

Presets export `compile_commands.json` into `build/<preset>/`, so clangd,
CLion and VS Code (CMake Tools extension) pick up the project automatically.
Point clangd at it with `--compile-commands-dir=build/dev` if needed.

## Day-to-day

| Task | Command |
|---|---|
| Format your changes | `scripts/format.sh` |
| Check formatting like CI | `scripts/format.sh --check` |
| Check commit messages like CI | `scripts/check_commit_messages.sh origin/main..HEAD` |
| Hunt memory bugs | `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` |
| Hunt data races | same with `tsan` |
| Run one test | `./build/dev/tests/vloom_engine_tests "[time]"` |

Next: read [architecture.md](architecture.md) and [coding-guidelines.md](coding-guidelines.md).
