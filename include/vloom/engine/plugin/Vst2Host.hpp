// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vloom {

/// One loaded VST 2 plugin file (ADR 0010). Control thread only.
///
/// A VST 2 file holds one plugin: a .dll on Windows, a .vst bundle on macOS,
/// a .so on Linux. To learn its name the plugin has to be created once, so
/// opening a file runs the plugin's own start-up code. Files that hold many
/// plugins ("shells") are refused.
///
/// On Windows a plugin can run in a bridge process instead of this one (ADR
/// 0011). A 32-bit plugin always does: open() finds the 32-bit bridge by
/// itself (vloom-plugin-bridge32.exe next to the running program, or the
/// file the VLOOM_PLUGIN_BRIDGE32 environment variable names).
///
/// Loading a plugin runs its code inside the host process with the host's
/// rights: open only files the user installed or chose. A library stays
/// loaded while it or any processor created from it lives; opening the same
/// file again gives the same library.
class Vst2Library : public std::enable_shared_from_this<Vst2Library> {
public:
    /// Loads `path` and reads what its plugin is. Null, with `error` set
    /// when given, if it is not a VST 2 plugin the host can use.
    static std::shared_ptr<Vst2Library> open(const std::filesystem::path& path, std::string* error = nullptr);
    /// The same, with the plugin in the bridge process `bridge` starts (a
    /// vloom-plugin-bridge executable): a crash of the plugin then ends the
    /// bridge, not the host, and the plugin goes silent. Windows only. A
    /// file can be open both ways at once: this never gives a library
    /// whose plugins run in the host.
    static std::shared_ptr<Vst2Library> openBridged(
        const std::filesystem::path& path, const std::filesystem::path& bridge, std::string* error = nullptr);

    ~Vst2Library();
    Vst2Library(const Vst2Library&) = delete;
    Vst2Library& operator=(const Vst2Library&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    /// Its plugin (one). The id is the plugin's own four-character id as 8
    /// hex digits.
    [[nodiscard]] const std::vector<PluginInfo>& plugins() const noexcept;
    /// True if its plugins run in a bridge process.
    [[nodiscard]] bool isBridged() const noexcept;

    /// Creates plugin `id` (the one of plugins()), ready for PluginInstance.
    /// Null, with `error` set when given, if the plugin refuses. The
    /// processor keeps the library loaded.
    [[nodiscard]] std::unique_ptr<AudioProcessor> create(std::string_view id, std::string* error = nullptr);

    struct Impl;

private:
    explicit Vst2Library(std::unique_ptr<Impl> impl);
    static std::shared_ptr<Vst2Library> open(
        const std::filesystem::path& path, std::filesystem::path bridge, std::string* error);
    std::unique_ptr<Impl> impl_;
};

/// Where VST 2 plugins are usually installed on this OS, the directories of
/// the VST_PATH environment variable first. There is no standard place on
/// Windows: the list holds the common ones and the one in the registry.
[[nodiscard]] std::vector<std::filesystem::path> vst2SearchPaths();

/// Every file under `directories` that may be a VST 2 plugin, sorted: .dll
/// on Windows (any DLL: only opening it tells), .vst bundles on macOS, .so
/// on Linux. Directories that are missing or unreadable are skipped.
[[nodiscard]] std::vector<std::filesystem::path> findVst2Files(
    const std::vector<std::filesystem::path>& directories);

} // namespace vloom
