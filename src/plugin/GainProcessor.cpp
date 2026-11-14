// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/GainProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vloom {

PluginInfo GainProcessor::info() const
{
    return PluginInfo{"vloom.gain", "Gain", "Vloom Audio", "1.0", PluginInfo::Kind::Effect};
}

std::vector<ParameterInfo> GainProcessor::parameters() const
{
    return {ParameterInfo{kGain, "Gain", "dB", kMinDb, kMaxDb, 0.0, false}};
}

bool GainProcessor::activate(double /*sampleRate*/, int /*maxBlockFrames*/)
{
    applied_ = linear(gainDb()); // no ramp from an old value
    return true;
}

std::vector<std::uint8_t> GainProcessor::saveState() const
{
    const double db = gainDb();
    std::vector<std::uint8_t> state(sizeof(double));
    std::memcpy(state.data(), &db, sizeof(double));
    return state;
}

bool GainProcessor::loadState(std::span<const std::uint8_t> state)
{
    if (state.size() != sizeof(double))
        return false;
    double db = 0.0;
    std::memcpy(&db, state.data(), sizeof(double));
    if (!std::isfinite(db))
        return false;
    // The audio thread ramps to it in its next block (applied_ is its own).
    db_.store(std::clamp(db, kMinDb, kMaxDb), std::memory_order_relaxed);
    return true;
}

float GainProcessor::linear(double db) noexcept
{
    if (db <= kMinDb)
        return 0.0f;
    return static_cast<float>(std::pow(10.0, std::min(db, kMaxDb) / 20.0));
}

void GainProcessor::process(const ProcessContext& context) noexcept
{
    for (const ParameterEvent& event : context.parameters)
        if (event.id == kGain && std::isfinite(event.value))
            db_.store(std::clamp(event.value, kMinDb, kMaxDb), std::memory_order_relaxed);
    const float target = linear(gainDb());
    const int frames = context.numFrames;
    if (frames <= 0)
        return;
    const float step = (target - applied_) / static_cast<float>(frames);
    for (int ch = 0; ch < context.numChannels; ++ch) {
        float* data = context.channels[ch];
        for (int i = 0; i < frames; ++i)
            data[i] *= applied_ + step * static_cast<float>(i + 1);
    }
    applied_ = target;
}

void GainProcessor::reset() noexcept { applied_ = linear(gainDb()); }

} // namespace vloom
