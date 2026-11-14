// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <atomic>

namespace vloom {

/// Built-in effect "vloom.gain": turns the level up or down by kGain dB
/// (-60 .. +12; at -60 it is silent). A change is ramped over the block in
/// which it arrives, so it does not click. The first built-in
/// AudioProcessor, and the reference for what an implementation must do.
class GainProcessor final : public AudioProcessor {
public:
    static constexpr std::uint32_t kGain = 0; ///< the parameter id
    static constexpr double kMinDb = -60.0;
    static constexpr double kMaxDb = 12.0;

    [[nodiscard]] PluginInfo info() const override;
    [[nodiscard]] std::vector<ParameterInfo> parameters() const override;
    bool activate(double sampleRate, int maxBlockFrames) override;
    [[nodiscard]] std::vector<std::uint8_t> saveState() const override;
    bool loadState(std::span<const std::uint8_t> state) override;
    void process(const ProcessContext& context) noexcept override;
    void reset() noexcept override;

    /// The gain in dB it is set to: by parameter changes (audio thread) or
    /// loadState() (control thread). Any thread may read it.
    [[nodiscard]] double gainDb() const noexcept { return db_.load(std::memory_order_relaxed); }

private:
    [[nodiscard]] static float linear(double db) noexcept;

    std::atomic<double> db_{0.0};
    float applied_ = 1.0f; ///< audio thread: the gain the last block ended at
};

} // namespace vloom
