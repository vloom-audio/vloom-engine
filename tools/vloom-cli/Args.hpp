// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace vloom::cli {

/// One channel-rack channel from `--channel <file.wav>:<pattern>`.
struct ChannelArg {
    std::string path;
    std::string pattern; ///< as typed: x = step on, . or - = off, | = ignored
};

struct Args {
    std::string command; ///< devices | play | render | --version
    std::string output; ///< output file for render
    int device = -1; ///< output device index, -1 = default
    double bpm = 120.0;
    double seconds = 0.0; ///< 0 = command default
    int rate = 48000;
    int buffer = 256;
    std::vector<ChannelArg> channels;
    std::optional<bool> click; ///< metronome; default: on unless channels are given
};

/// Parses argv. Returns std::nullopt on bad input (usage should be printed).
std::optional<Args> parseArgs(int argc, char** argv);

void printUsage();

} // namespace vloom::cli
