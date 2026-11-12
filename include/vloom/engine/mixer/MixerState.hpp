// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vloom {

/// Mixer inserts the engine routes channels to, 1..kMaxInserts (0 = straight
/// to the master), as in FL Studio.
inline constexpr int kMaxInserts = 125;

/// Most frames process() renders in one go; larger requests are split.
/// Per-channel and per-insert buffers have this size.
inline constexpr int kMaxBlockFrames = 4096;

/// One insert, as the audio thread sees it. The host folds mute and solo into
/// `gain` (0 = silent).
struct InsertState {
    float gain = 1.0f; ///< linear, 0 .. 4
    float pan = 0.0f; ///< balance, -1 (left) .. +1 (right)
};

/// Immutable snapshot of the mixer, published with Engine::setMixerState().
/// `inserts[i]` is insert i + 1; inserts past the end play at unity.
struct MixerState {
    std::vector<InsertState> inserts;
};

/// Left and right gain for `gain` and balance `pan`: the side panned away
/// from is turned down, the other keeps `gain` (as the master pan does).
/// Non-finite input gives silence; gain is clamped to 0 .. 4.
struct StereoGain {
    float left = 1.0f;
    float right = 1.0f;
};
[[nodiscard]] inline StereoGain balance(float gain, float pan) noexcept
{
    if (!std::isfinite(gain) || !std::isfinite(pan))
        return {0.0f, 0.0f};
    gain = std::clamp(gain, 0.0f, 4.0f);
    pan = std::clamp(pan, -1.0f, 1.0f);
    return {gain * std::min(1.0f, 1.0f - pan), gain * std::min(1.0f, 1.0f + pan)};
}

} // namespace vloom
