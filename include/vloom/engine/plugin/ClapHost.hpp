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

/// One loaded .clap file and the plugins it holds (ADR 0006). Control thread
/// only.
///
/// Loading a .clap runs its code inside the host process with the host's
/// rights: open only files the user installed or chose. A library stays
/// loaded while it or any processor created from it lives; opening the same
/// file again gives the same library.
class ClapLibrary : public std::enable_shared_from_this<ClapLibrary> {
public:
    /// Loads `path` (a .clap file; on macOS the bundle directory) and lists
    /// its plugins. Null, with `error` set when given, if it is not a CLAP
    /// library the host can use.
    static std::shared_ptr<ClapLibrary> open(const std::filesystem::path& path, std::string* error = nullptr);

    ~ClapLibrary();
    ClapLibrary(const ClapLibrary&) = delete;
    ClapLibrary& operator=(const ClapLibrary&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::vector<PluginInfo>& plugins() const noexcept;

    /// Creates plugin `id` (one of plugins()), ready for PluginInstance.
    /// Null, with `error` set when given, if the plugin refuses. The
    /// processor keeps the library loaded.
    [[nodiscard]] std::unique_ptr<AudioProcessor> create(std::string_view id, std::string* error = nullptr);

    struct Impl;

private:
    explicit ClapLibrary(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Where CLAP plugins are installed on this OS (the CLAP spec's list), the
/// directories of the CLAP_PATH environment variable first.
[[nodiscard]] std::vector<std::filesystem::path> clapSearchPaths();

/// Every .clap under `directories` (recursively; a macOS bundle counts as
/// one file), sorted. Directories that are missing or unreadable are skipped.
[[nodiscard]] std::vector<std::filesystem::path> findClapFiles(
    const std::vector<std::filesystem::path>& directories);

} // namespace vloom
