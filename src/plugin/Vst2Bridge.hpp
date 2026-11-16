// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <filesystem>
#include <memory>
#include <string>

// The engine's side of the plugin bridge (ADR 0011): a VST 2 plugin that
// runs in a vloom-plugin-bridge process, as an AudioProcessor. Windows only;
// elsewhere create() fails and nothing is ever found.
namespace vloom::vst2_bridge {

/// Starts `bridge` (a vloom-plugin-bridge executable) with the plugin in
/// `plugin`. Null, with `error` set when given, if the bridge does not start
/// or the plugin does not load in it. The processor ends the bridge when it
/// is destroyed.
[[nodiscard]] std::unique_ptr<AudioProcessor> create(
    const std::filesystem::path& bridge, const std::filesystem::path& plugin, std::string* error);

/// True if `file` is a Windows module for 32-bit x86 while this process is
/// not: one that only a 32-bit bridge can load.
[[nodiscard]] bool needs32BitBridge(const std::filesystem::path& file);

/// The 32-bit bridge: the file the VLOOM_PLUGIN_BRIDGE32 environment
/// variable names, or vloom-plugin-bridge32.exe next to the running
/// program. Empty if there is none.
[[nodiscard]] std::filesystem::path find32BitBridge();

} // namespace vloom::vst2_bridge
