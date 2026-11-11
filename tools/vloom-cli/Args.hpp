// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <optional>
#include <string>

namespace vloom::cli {

struct Args {
    std::string command; ///< devices | play | render | --version
    std::string output; ///< output file for render
    int device = -1; ///< output device index, -1 = default
    double bpm = 120.0;
    double seconds = 0.0; ///< 0 = command default
    int rate = 48000;
    int buffer = 256;
};

/// Parses argv. Returns std::nullopt on bad input (usage should be printed).
std::optional<Args> parseArgs(int argc, char** argv);

void printUsage();

} // namespace vloom::cli
