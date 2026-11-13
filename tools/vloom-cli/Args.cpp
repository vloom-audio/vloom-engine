// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "Args.hpp"

#include <vloom/engine/Engine.hpp>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace vloom::cli {

void printUsage()
{
    std::puts("usage:\n"
              "  vloom-cli devices\n"
              "  vloom-cli play   [--device N] [--bpm 120] [--seconds 8] [--buffer 256] [channels]\n"
              "  vloom-cli render <out.wav> [--bpm 120] [--seconds 4] [--rate 48000]\n"
              "                   [--format float|pcm16|pcm24] [channels]\n"
              "  vloom-cli --version\n"
              "\n"
              "channels (channel rack, 16th-note steps, the pattern loops):\n"
              "  --channel <file.wav>:<pattern>   e.g. --channel kick.wav:x...x...x...x...\n"
              "                                   x = play, . or - = rest, | = ignored; repeatable\n"
              "  --click / --no-click             metronome (default: on only without channels)");
}

namespace {

/// The whole of `text` as an int in [lo, hi]; nullopt otherwise.
std::optional<int> parseInt(std::string_view text, int lo, int hi)
{
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < lo || value > hi)
        return std::nullopt;
    return value;
}

/// The whole of `text` as a finite double in [lo, hi]; nullopt otherwise.
std::optional<double> parseNumber(const char* text, double lo, double hi)
{
    // strtod rather than from_chars: floating-point from_chars is missing
    // from older standard libraries (Apple's).
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value) || value < lo || value > hi)
        return std::nullopt;
    return value;
}

void complain(const char* option, const char* value, const char* expected)
{
    std::fprintf(stderr, "%s %s: expected %s\n", option, value, expected);
}

} // namespace

std::optional<Args> parseArgs(int argc, char** argv)
{
    if (argc < 2)
        return std::nullopt;

    Args args;
    args.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;

        if (arg == "--device" && hasValue) {
            const char* value = argv[++i];
            const auto device = parseInt(value, 0, 1000);
            if (!device) {
                complain("--device", value, "a device number from 'vloom-cli devices'");
                return std::nullopt;
            }
            args.device = *device;
        } else if (arg == "--bpm" && hasValue) {
            const char* value = argv[++i];
            const auto bpm = parseNumber(value, 10.0, 999.0);
            if (!bpm) {
                complain("--bpm", value, "a tempo from 10 to 999");
                return std::nullopt;
            }
            args.bpm = *bpm;
        } else if (arg == "--seconds" && hasValue) {
            const char* value = argv[++i];
            const auto seconds = parseNumber(value, 0.001, 24.0 * 3600.0);
            if (!seconds) {
                complain("--seconds", value, "a length from 0.001 to 86400 seconds");
                return std::nullopt;
            }
            args.seconds = *seconds;
        } else if (arg == "--rate" && hasValue) {
            const char* value = argv[++i];
            const auto rate = parseInt(
                value, static_cast<int>(Engine::kMinSampleRate), static_cast<int>(Engine::kMaxSampleRate));
            if (!rate) {
                complain("--rate", value, "a sample rate from 8000 to 768000 Hz");
                return std::nullopt;
            }
            args.rate = *rate;
        } else if (arg == "--buffer" && hasValue) {
            const char* value = argv[++i];
            const auto buffer = parseInt(value, 16, 8192);
            if (!buffer) {
                complain("--buffer", value, "a buffer size from 16 to 8192 frames");
                return std::nullopt;
            }
            args.buffer = *buffer;
        } else if (arg == "--format" && hasValue) {
            const char* value = argv[++i];
            const std::string_view format = value;
            if (format != "float" && format != "pcm16" && format != "pcm24") {
                complain("--format", value, "float, pcm16 or pcm24");
                return std::nullopt;
            }
            args.format = std::string(format);
        } else if (arg == "--channel" && hasValue) {
            // The pattern follows the last ':', so Windows paths (C:\...) work.
            const std::string value = argv[++i];
            const auto colon = value.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == value.size()) {
                std::fprintf(stderr, "--channel needs <file.wav>:<pattern>, got: %s\n", value.c_str());
                return std::nullopt;
            }
            args.channels.push_back({value.substr(0, colon), value.substr(colon + 1)});
        } else if (arg == "--click")
            args.click = true;
        else if (arg == "--no-click")
            args.click = false;
        else if (!arg.starts_with("--") && args.output.empty())
            args.output = std::string(arg);
        else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return std::nullopt;
        }
    }
    return args;
}

} // namespace vloom::cli
