// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>
#include <vector>

namespace vloom {

class SampleBuffer;

/// Sample player with a fixed pool of voices: one-shots, or notes held until
/// released, at any pitch. Real-time safe: nothing but the constructor
/// allocates, locks or waits.
///
/// At rate 1 samples play unchanged: resample them to the engine's rate
/// first (sample/Resampler). Other rates read them with 4-point Hermite
/// interpolation. The sampler keeps raw pointers to them, so
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

    /// Starts `sample` from frame `startFrame` (0: its beginning). `gain` is
    /// linear; `pan` is -1 (left) .. +1 (right): constant power for mono
    /// samples, balance for stereo ones. A null or empty sample, or a start
    /// at or past its end, is ignored. A start inside the sample (playback
    /// resumed in the middle of a playlist clip) fades in over kFadeFrames so
    /// it does not click.
    void trigger(const SampleBuffer* sample, float gain = 1.0f, float pan = 0.0f,
        std::int64_t startFrame = 0) noexcept;

    static constexpr std::int64_t kNoRelease = INT64_MAX;
    /// Length of a note's release (10 ms at 48 kHz).
    static constexpr int kReleaseFrames = 480;
    static constexpr double kMinRate = 1.0 / 16.0;
    static constexpr double kMaxRate = 16.0;

    /// How play() starts a sample.
    struct PlayOptions {
        float gain = 1.0f; ///< linear
        float pan = 0.0f; ///< -1 .. +1, as for trigger()
        std::int64_t startFrame = 0; ///< as for trigger()
        /// Playback speed: 2 = an octave up, 0.5 = an octave down; clamped to
        /// kMinRate .. kMaxRate. Other than 1 the sample is interpolated.
        double rate = 1.0;
        /// The rate is also scaled by setPitch() (a master pitch); off for
        /// audio that must stay in time with the song.
        bool followsPitch = true;
        /// A held note: release(t) with t >= releaseAt starts its release
        /// (kReleaseFrames). kNoRelease: it plays to the end of the sample.
        /// The unit is the caller's (the sequencer uses ticks).
        std::int64_t releaseAt = kNoRelease;
    };

    /// Like trigger(), with pitch and note length.
    void play(const SampleBuffer* sample, const PlayOptions& options) noexcept;

    /// Speed factor for the voices that follow the pitch
    /// (PlayOptions::followsPitch), sounding ones included; clamped to
    /// kMinRate .. kMaxRate, 1 = none.
    void setPitch(double ratio) noexcept;
    [[nodiscard]] double pitch() const noexcept { return pitch_; }

    /// The smallest releaseAt among the held notes still sounding (not
    /// already releasing); kNoRelease when there is none.
    [[nodiscard]] std::int64_t nextRelease() const noexcept;
    /// Releases the held notes whose releaseAt is at or before `upTo`.
    void release(std::int64_t upTo) noexcept;
    /// Releases every held note, whatever its releaseAt (a jump in the song).
    void releaseHeld() noexcept;

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
        double fraction = 0.0; ///< how far past `position`, 0 .. 1
        double rate = 1.0;
        bool followsPitch = true;
        std::int64_t releaseAt = kNoRelease;
        float gainLeft = 0.0f;
        float gainRight = 0.0f;
        bool fading = false; ///< fading out (stolen, stopped or released)
        int fadeLeft = 0; ///< while fading: frames to go
        int fadeLength = kFadeFrames; ///< while fading: the whole fade
        int fadeInLeft = 0; ///< frames of fade-in to go (started inside the sample)
        std::uint64_t age = 0; ///< trigger order, to find the oldest
    };

    static constexpr int kFadeSlots = 4; ///< extra slots for voices fading out

    void renderVoice(
        Voice& voice, float* const* out, int numChannels, int startFrame, int numFrames) noexcept;

    /// Starts fading `voice` out over `frames`, unless it already ends sooner.
    static void startFade(Voice& voice, int frames) noexcept;

    int maxVoices_;
    double pitch_ = 1.0;
    std::vector<Voice> voices_; ///< maxVoices_ + kFadeSlots, sized once
    std::uint64_t triggers_ = 0;
};

} // namespace vloom
