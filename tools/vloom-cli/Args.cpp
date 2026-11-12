// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "Args.hpp"

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
              "  vloom-cli render <out.wav> [--bpm 120] [--seconds 4] [--rate 48000] [channels]\n"
              "  vloom-cli --version\n"
              "\n"
              "channels (channel rack, 16th-note steps, the pattern loops):\n"
              "  --channel <file.wav>:<pattern>   e.g. --channel kick.wav:x...x...x...x...\n"
              "                                   x = play, . or - = rest, | = ignored; repeatable\n"
              "  --click / --no-click             metronome (default: on only without channels)");
}

std::optional<Args> parseArgs(int argc, char** argv)
{
    if (argc < 2)
        return std::nullopt;

    Args args;
    args.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;

        if (arg == "--device" && hasValue)
            args.device = std::atoi(argv[++i]);
        else if (arg == "--bpm" && hasValue)
            args.bpm = std::atof(argv[++i]);
        else if (arg == "--seconds" && hasValue)
            args.seconds = std::atof(argv[++i]);
        else if (arg == "--rate" && hasValue)
            args.rate = std::atoi(argv[++i]);
        else if (arg == "--buffer" && hasValue)
            args.buffer = std::atoi(argv[++i]);
        else if (arg == "--channel" && hasValue) {
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
    if (!(args.bpm > 0.0) || !std::isfinite(args.bpm)) {
        std::fprintf(stderr, "--bpm must be a positive number\n");
        return std::nullopt;
    }
    return args;
}

} // namespace vloom::cli
