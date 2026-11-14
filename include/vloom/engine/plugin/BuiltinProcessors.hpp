// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace vloom {

/// The processors built into the engine (ADR 0008). Their sound comes from
/// vloom-dsp; the engine wraps each in an AudioProcessor.
///
/// Built-in state is "VLB1", a 4-byte count, then per parameter its 4-byte
/// id and 8-byte value (little endian). Unknown ids are skipped, so state
/// from a newer version still loads.
[[nodiscard]] std::vector<PluginInfo> builtinProcessors();

/// A new instance of the built-in processor `id` (e.g. "vloom.gain"), or
/// nullptr if there is none. Control thread.
[[nodiscard]] std::unique_ptr<AudioProcessor> createBuiltinProcessor(std::string_view id);

} // namespace vloom
