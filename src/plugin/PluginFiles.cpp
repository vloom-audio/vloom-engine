// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/PluginFiles.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace vloom::plugin_files {

void fail(std::string* error, std::string message)
{
    if (error != nullptr)
        *error = std::move(message);
}

bool SharedLibrary::load(const std::filesystem::path& file, std::string* error)
{
    close();
#ifdef _WIN32
    handle_ = ::LoadLibraryW(file.c_str());
    if (handle_ == nullptr) {
        fail(error, "cannot load " + file.string() + " (error " + std::to_string(::GetLastError()) + ")");
        return false;
    }
#else
    handle_ = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char* reason = ::dlerror();
        fail(error, "cannot load " + file.string() + (reason != nullptr ? ": " + std::string(reason) : ""));
        return false;
    }
#endif
    return true;
}

void* SharedLibrary::symbol(const char* name) const
{
    if (handle_ == nullptr)
        return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(::GetProcAddress(handle_, name));
#else
    return ::dlsym(handle_, name);
#endif
}

void SharedLibrary::close()
{
    if (handle_ == nullptr)
        return;
#ifdef _WIN32
    ::FreeLibrary(handle_);
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
}

std::string environment(const char* name)
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::vector<std::filesystem::path> pathsFrom(const char* name)
{
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::vector<std::filesystem::path> paths;
    const std::string list = environment(name);
    for (std::size_t start = 0; start <= list.size();) {
        const std::size_t end = std::min(list.find(kSeparator, start), list.size());
        if (end > start)
            paths.emplace_back(list.substr(start, end - start));
        start = end + 1;
    }
    return paths;
}

std::vector<std::filesystem::path> findFiles(
    const std::vector<std::filesystem::path>& directories, const std::string& extension)
{
    std::vector<std::filesystem::path> found;
    for (const auto& directory : directories) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            directory, std::filesystem::directory_options::skip_permission_denied, ec);
        for (const std::filesystem::recursive_directory_iterator end{}; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != extension)
                continue;
            found.push_back(it->path());
            if (it->is_directory(ec))
                it.disable_recursion_pending(); // a bundle: one plugin file
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

} // namespace vloom::plugin_files
