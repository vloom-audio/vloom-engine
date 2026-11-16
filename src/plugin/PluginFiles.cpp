// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/PluginFiles.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace vloom::plugin_files {

void fail(std::string* error, std::string message)
{
    if (error != nullptr)
        *error = std::move(message);
}

std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

namespace {

/// True if `text` is well-formed UTF-8 with no control characters.
bool cleanUtf8(const std::string& text)
{
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t more = 0;
        std::uint32_t code = lead;
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7F)
                return false;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            code = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (text.size() - i <= more)
            return false;
        for (std::size_t k = 1; k <= more; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3Fu);
        }
        // No overlong forms, no surrogates, nothing past the last code point.
        if ((more == 2 && code < 0x800) || (more == 3 && code < 0x10000) || (code >= 0xD800 && code <= 0xDFFF)
            || code > 0x10FFFF || (code >= 0x80 && code < 0xA0))
            return false;
        i += more + 1;
    }
    return true;
}

std::uint32_t le16(const unsigned char* p) { return static_cast<std::uint32_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const unsigned char* p) { return le16(p) | (le16(p + 2) << 16); }

} // namespace

std::string printable(std::string text)
{
    if (!cleanUtf8(text))
        for (char& c : text)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                c = '?';
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

ModuleFacts readModule(const std::filesystem::path& file, std::initializer_list<std::string_view> exportNames)
{
    ModuleFacts facts;
    std::ifstream in(file, std::ios::binary);
    // `count` bytes at `offset` of the file; false if they are not there.
    const auto read = [&in](std::uint64_t offset, unsigned char* to, std::size_t count) {
        in.clear();
        return static_cast<bool>(in.seekg(static_cast<std::streamoff>(offset)))
            && static_cast<bool>(in.read(reinterpret_cast<char*>(to), static_cast<std::streamsize>(count)));
    };
    std::array<unsigned char, 64> dos{};
    if (!in || !read(0, dos.data(), dos.size()) || dos[0] != 'M' || dos[1] != 'Z')
        return facts;
    const std::uint64_t pe = le32(dos.data() + 0x3C);
    // "PE\0\0", then the 20 bytes that say what it is for and how large the rest is.
    std::array<unsigned char, 24> coff{};
    if (!read(pe, coff.data(), coff.size()) || coff[0] != 'P' || coff[1] != 'E' || coff[2] != 0
        || coff[3] != 0)
        return facts;
    facts.module = true;
    facts.machine = static_cast<std::uint16_t>(le16(coff.data() + 4));
    if (exportNames.size() == 0)
        return facts;
    const std::uint32_t sectionCount = std::min<std::uint32_t>(le16(coff.data() + 6), 96);
    const std::uint32_t optionalSize = le16(coff.data() + 20);
    const std::uint64_t optional = pe + coff.size();
    std::array<unsigned char, 2> magic{};
    if (!read(optional, magic.data(), magic.size()))
        return facts;
    // Where the table of exports is said to be: at a different place in a
    // 32-bit and a 64-bit module.
    const std::uint32_t directories = le16(magic.data()) == 0x020B ? 112u : 96u;
    std::array<unsigned char, 8> exportEntry{};
    if (optionalSize < directories + exportEntry.size()
        || !read(optional + directories, exportEntry.data(), exportEntry.size()))
        return facts;
    // The file holds the module in sections; an address in the loaded
    // module is turned into a place in the file through them.
    struct Section {
        std::uint32_t address, size, fileOffset, fileSize;
    };
    std::vector<Section> sections;
    for (std::uint32_t i = 0; i < sectionCount; ++i) {
        std::array<unsigned char, 40> header{};
        if (!read(optional + optionalSize + std::uint64_t{i} * header.size(), header.data(), header.size()))
            return facts;
        sections.push_back({le32(header.data() + 12), le32(header.data() + 8), le32(header.data() + 20),
            le32(header.data() + 16)});
    }
    const auto place = [&sections](std::uint32_t address, std::uint64_t& offset) {
        for (const Section& s : sections)
            if (address >= s.address && address - s.address < s.fileSize) {
                offset = std::uint64_t{s.fileOffset} + (address - s.address);
                return true;
            }
        return false;
    };
    std::uint64_t table = 0;
    std::array<unsigned char, 40> exports{};
    if (le32(exportEntry.data()) == 0 || !place(le32(exportEntry.data()), table)
        || !read(table, exports.data(), exports.size()))
        return facts;
    const std::uint32_t nameCount = std::min<std::uint32_t>(le32(exports.data() + 24), 65536);
    std::uint64_t names = 0;
    if (!place(le32(exports.data() + 32), names))
        return facts;
    for (std::uint32_t i = 0; i < nameCount && !facts.exports; ++i) {
        std::array<unsigned char, 4> entry{};
        std::uint64_t at = 0;
        if (!read(names + std::uint64_t{i} * 4, entry.data(), entry.size()) || !place(le32(entry.data()), at))
            return facts;
        // Long enough for the names asked for, and one more to see the end.
        std::array<unsigned char, 64> name{};
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(name.data()), static_cast<std::streamsize>(name.size() - 1));
        const std::string_view found(reinterpret_cast<const char*>(name.data()));
        for (const std::string_view wanted : exportNames)
            facts.exports = facts.exports || found == wanted;
    }
    return facts;
}

std::string hex8(std::uint32_t value)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i, value >>= 4)
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
    return out;
}

bool SharedLibrary::load(const std::filesystem::path& file, std::string* error)
{
    close();
    const std::string name = utf8(file);
#ifdef _WIN32
    // A file that is not a DLL (or one whose own DLLs are missing) makes
    // Windows show a "Bad Image" message box and wait for the user, unless
    // the thread asks for the error to be returned instead. Without this a
    // damaged plugin file stops the app, and a test of one hangs CI.
    DWORD previousMode = 0;
    const bool modeSet = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode) != 0;
    // With the module's own folder searched first for the DLLs it brings
    // along (a full path is needed for that; relative ones load as before).
    handle_ = file.is_absolute() ? ::LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
                                 : ::LoadLibraryW(file.c_str());
    const DWORD code = ::GetLastError();
    if (modeSet)
        ::SetThreadErrorMode(previousMode, nullptr);
    if (handle_ == nullptr) {
        fail(error, "cannot load " + name + " (error " + std::to_string(code) + ")");
        return false;
    }
#else
    handle_ = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char* reason = ::dlerror();
        fail(error, "cannot load " + name + (reason != nullptr ? ": " + std::string(reason) : ""));
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
