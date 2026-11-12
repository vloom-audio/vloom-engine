// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>
#include <vector>

namespace vloom {

class SampleBuffer;

/// One-shot sample player with a fixed pool of voices. Real-time safe:
/// trigger(), render() and stopAll() never allocate, lock or wait.
///
/// Samples play at their own rate unchanged: resample them to the engine's
/// rate first (sample/Resampler). The sampler keeps raw pointers to them, so
/// a SampleBuffer must outlive every voice playing it; uses() tells whether
/// any voice (also one fading out) still reads it.
///
/// Up to maxVoices() notes sound at once (the polyphony, set per channel);
/// a further trigger fades out the oldest instead of cutting it, so nothing
/// clicks. With 1 voice each note cuts the previous one (hi-hats).
///
/// Construct it on a control thread: the voice pool is allocated there.
class Sampler {
public:
    static constexpr int kDefaultVoices = 16;
    static constexpr int kMaxPolyphony = 64;
    /// Length of the fade when a voice is stolen or stopped (about 1.3 ms at 48 kHz).
    static constexpr int kFadeFrames = 64;

    /// `maxVoices` is clamped to 1 .. kMaxPolyphony.
    explicit Sampler(int maxVoices = kDefaultVoices);

    [[nodiscard]] int maxVoices() const noexcept { return maxVoices_; }

    /// Silences every voice.
    void reset() noexcept;

    /// Starts `sample` from its beginning. `gain` is linear; `pan` is -1
    /// (left) .. +1 (right): constant power for mono samples, balance for
    /// stereo ones. A null or empty sample is ignored.
    void trigger(const SampleBuffer* sample, float gain = 1.0f, float pan = 0.0f) noexcept;

    /// Adds every voice into `out[ch][startFrame .. startFrame + numFrames)`.
    /// Mono samples go to both of the first two channels; channels after the
    /// second get nothing.
    void render(float* const* out, int numChannels, int startFrame, int numFrames) noexcept;

    /// Fades out every voice over kFadeFrames.
    void stopAll() noexcept;

    [[nodiscard]] bool uses(const SampleBuffer* sample) const noexcept;
    /// Voices still sounding, fading ones included.
    [[nodiscard]] int activeVoices() const noexcept;

private:
    struct Voice {
        const SampleBuffer* sample = nullptr; ///< null = free
        std::int64_t position = 0; ///< next frame of the sample to play
        float gainLeft = 0.0f;
        float gainRight = 0.0f;
        bool fading = false; ///< fading out (stolen or stopped)
        int fadeLeft = 0; ///< while fading: frames to go
        std::uint64_t age = 0; ///< trigger order, to find the oldest
    };

    static constexpr int kFadeSlots = 4; ///< extra slots for voices fading out

    void renderVoice(
        Voice& voice, float* const* out, int numChannels, int startFrame, int numFrames) noexcept;

    int maxVoices_;
    std::vector<Voice> voices_; ///< maxVoices_ + kFadeSlots, sized once
    std::uint64_t triggers_ = 0;
};

} // namespace vloom
