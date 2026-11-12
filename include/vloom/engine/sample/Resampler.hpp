// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/sample/SampleBuffer.hpp"

namespace vloom {

/// Converts `input` to `targetRate` offline (on a control or background
/// thread, never the audio thread), so samples play at the engine's rate.
///
/// Band-limited windowed-sinc interpolation (Kaiser window, 32 zero
/// crossings per side). When lowering the rate the filter cutoff follows the
/// new Nyquist frequency, so content above it is removed instead of aliasing.
/// The output has round(frames * targetRate / inputRate) frames. Same rate:
/// a copy. A target or input rate <= 0 gives an empty buffer.
[[nodiscard]] SampleBuffer resample(const SampleBuffer& input, double targetRate);

} // namespace vloom
