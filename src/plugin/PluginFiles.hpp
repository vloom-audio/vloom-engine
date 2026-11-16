// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// Engine-internal helpers shared by the plugin format hosts (CLAP, VST3):
// loading a shared library and finding plugin files.

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vloom::plugin_files {

/// Sets `*error` to `message` when `error` is given.
void fail(std::string* error, std::string message);

/// `path` as UTF-8, for messages and for plugins (CLAP takes its path so).
/// path::string() throws on Windows for names outside the system code page.
[[nodiscard]] std::string utf8(const std::filesystem::path& path);

/// A name from a plugin that declares no encoding, made safe to show: kept
/// as it is if it is valid UTF-8 without control characters, otherwise with
/// everything but printable ASCII replaced by '?'. No spaces at its end.
[[nodiscard]] std::string printable(std::string text);

/// What a Windows module file (.dll, .exe) says in its headers.
struct ModuleFacts {
    bool module = false; ///< it has the headers of one
    std::uint16_t machine = 0; ///< what it is built for: 0x014C x86, 0x8664 x64, 0xAA64 ARM64
    bool exports = false; ///< it exports one of the names asked for
};

/// Reads `file`'s headers and its list of exported names, without loading
/// it: none of its code runs. Every offset in the file is checked.
[[nodiscard]] ModuleFacts readModule(
    const std::filesystem::path& file, std::initializer_list<std::string_view> exportNames);

/// `value` as 8 upper-case hex digits.
[[nodiscard]] std::string hex8(std::uint32_t value);

/// A loaded shared library: a .clap, or the binary inside a .vst3.
class SharedLibrary {
public:
    SharedLibrary() = default;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    ~SharedLibrary() { close(); }

    bool load(const std::filesystem::path& file, std::string* error);
    [[nodiscard]] void* symbol(const char* name) const;
    /// The OS handle (dlopen's or LoadLibrary's), for entry points that want it.
    [[nodiscard]] void* handle() const noexcept { return reinterpret_cast<void*>(handle_); }
    void close();

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

/// An environment variable, or empty.
[[nodiscard]] std::string environment(const char* name);

/// The directories listed in environment variable `name` (separated as PATH is).
[[nodiscard]] std::vector<std::filesystem::path> pathsFrom(const char* name);

/// Every file or bundle with `extension` (e.g. ".clap") under `directories`,
/// recursively; a bundle directory counts as one file. Sorted, no
/// duplicates; missing or unreadable directories are skipped.
[[nodiscard]] std::vector<std::filesystem::path> findFiles(
    const std::vector<std::filesystem::path>& directories, const std::string& extension);

} // namespace vloom::plugin_files
