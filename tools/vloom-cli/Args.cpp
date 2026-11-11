// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "Args.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace vloom::cli {

void printUsage()
{
    std::puts("usage:\n"
              "  vloom-cli devices\n"
              "  vloom-cli play   [--device N] [--bpm 120] [--seconds 8] [--buffer 256]\n"
              "  vloom-cli render <out.wav> [--bpm 120] [--seconds 4] [--rate 48000]\n"
              "  vloom-cli --version");
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
