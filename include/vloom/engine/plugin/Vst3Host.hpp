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

/// One loaded VST 3 module (a .vst3 bundle or file) and the audio plugins it
/// holds (ADR 0006). Control thread only.
///
/// Like a .clap, a .vst3 runs its code inside the host process with the
/// host's rights: open only modules the user installed or chose. A module
/// stays loaded while it or any processor created from it lives; opening
/// the same path again gives the same library.
///
/// The adapter speaks VST 3 as a host: component and edit controller (one
/// object or two, connected), 32-bit stereo main buses, an event input for
/// instruments, parameter changes, the process context, latency and state.
/// Parameters are VST 3's normalized values: 0 .. 1.
class Vst3Library : public std::enable_shared_from_this<Vst3Library> {
public:
    /// Loads `path` (a .vst3 bundle directory, or a single .vst3 file) and
    /// lists its audio plugins. Null, with `error` set when given, if it is
    /// not a VST 3 module the host can use.
    static std::shared_ptr<Vst3Library> open(const std::filesystem::path& path, std::string* error = nullptr);

    ~Vst3Library();
    Vst3Library(const Vst3Library&) = delete;
    Vst3Library& operator=(const Vst3Library&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    /// Its audio processors. A plugin's id is its class id as 32 hex digits.
    [[nodiscard]] const std::vector<PluginInfo>& plugins() const noexcept;

    /// Creates plugin `id` (one of plugins()), ready for PluginInstance.
    /// Null, with `error` set when given, if the plugin refuses. The
    /// processor keeps the module loaded.
    [[nodiscard]] std::unique_ptr<AudioProcessor> create(std::string_view id, std::string* error = nullptr);

    struct Impl;

private:
    explicit Vst3Library(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Where VST 3 plugins are installed on this OS (Steinberg's list), the
/// directories of the VST3_PATH environment variable first.
[[nodiscard]] std::vector<std::filesystem::path> vst3SearchPaths();

/// Every .vst3 under `directories` (recursively; a bundle counts as one
/// file), sorted. Directories that are missing or unreadable are skipped.
[[nodiscard]] std::vector<std::filesystem::path> findVst3Files(
    const std::vector<std::filesystem::path>& directories);

} // namespace vloom
