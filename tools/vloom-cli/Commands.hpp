// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "Args.hpp"

namespace vloom::cli {

// Each command returns a process exit code.
int listDevices();
int play(const Args& args);
int render(const Args& args);

} // namespace vloom::cli
