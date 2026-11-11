// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// vloom-cli — command-line front end for vloom-engine.
// Run without arguments for usage. To add a command: declare it in
// Commands.hpp, implement it in Commands.cpp, dispatch it below.

#include "Args.hpp"
#include "Commands.hpp"

#include <vloom/engine/core/Version.hpp>

#include <cstdio>

int main(int argc, char** argv)
{
    const auto args = vloom::cli::parseArgs(argc, argv);
    if (!args) {
        vloom::cli::printUsage();
        return 2;
    }

    if (args->command == "--version") {
        std::printf("vloom-engine %s\n", vloom::engineVersion());
        return 0;
    }
    if (args->command == "devices")
        return vloom::cli::listDevices();
    if (args->command == "play")
        return vloom::cli::play(*args);
    if (args->command == "render")
        return vloom::cli::render(*args);

    vloom::cli::printUsage();
    return 2;
}
