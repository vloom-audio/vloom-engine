// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/core/Version.hpp"

namespace vloom {

const char* engineVersion() noexcept
{
    return VLOOM_ENGINE_VERSION; // defined by CMake from project(VERSION)
}

} // namespace vloom
