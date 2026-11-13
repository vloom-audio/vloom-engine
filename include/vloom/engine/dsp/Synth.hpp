// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>
#include <vector>

namespace vloom {

enum class Waveform : std::uint8_t { Sine, Triangle, Saw, Square };

/// The sound of a Synth channel. Plain values: the settings travel in each
/// SequencerState snapshot and apply live, sounding notes included. Values
/// out of range are clamped, non-finite ones replaced by the defaults.
struct SynthSettings {
    Waveform waveform = Waveform::Saw;
    float attack = 0.005f; ///< seconds, kMinAttack .. kMaxTime
    float decay = 0.3f; ///< seconds, kMinAttack .. kMaxTime
    float sustain = 0.6f; ///< level 0 .. 1
    float release = 0.2f; ///< seconds, kMinRelease .. kMaxTime
    float cutoff = 6000.0f; ///< low-pass, Hz, kMinCutoff .. kMaxCutoff (and below Nyquist)
    float resonance = 0.2f; ///< 0 .. 1 (1 rings, never self-oscillates)
    float level = 0.5f; ///< output gain, 0 .. 1

    static constexpr float kMinAttack = 0.001f; ///< also the shortest decay
    static constexpr float kMinRelease = 0.002f;
    static constexpr float kMaxTime = 10.0f;
    static constexpr float kMinCutoff = 20.0f;
    static constexpr float kMaxCutoff = 20000.0f;
};

/// The first built-in instrument: a polyphonic subtractive synth. One
/// oscillator per voice (sine, triangle, or band-limited saw and square via
/// polyBLEP), a linear ADSR envelope and a resonant low-pass filter
/// (topology-preserving state-variable filter). Mono: both output channels
/// get the same signal, -3 dB like a mono sample in the centre.
///
/// The voice interface mirrors Sampler: notes are held until release(), a
/// further note beyond the polyphony fades out the oldest, setPitch() is a
/// live master pitch. Real-time safe: nothing but the constructor
/// allocates, locks or waits. Construct it on a control thread.
class Synth {
public:
    static constexpr int kDefaultVoices = 16;
    static constexpr int kMaxPolyphony = 64;
    /// Length of the fade when a voice is stolen or stopped.
    static constexpr int kFadeFrames = 64;
    static constexpr std::int64_t kNoRelease = INT64_MAX;

    /// `maxVoices` is clamped to 1 .. kMaxPolyphony.
    explicit Synth(int maxVoices = kDefaultVoices);

    [[nodiscard]] int maxVoices() const noexcept { return maxVoices_; }
    /// The output rate (default 48 kHz), clamped to 1 kHz .. 768 kHz.
    /// Sounding voices keep their pitch.
    void setSampleRate(double sampleRate) noexcept;
    /// Silences every voice at once.
    void reset() noexcept;

    /// Starts a note at `frequency` Hz (scaled by setPitch()), `velocity`
    /// 0 .. 1 scaling its level. `releaseAt` as Sampler::PlayOptions:
    /// release() reaching it starts the envelope's release; kNoRelease holds
    /// the note until releaseHeld() or stopAll(). Non-finite or non-positive
    /// frequencies are ignored.
    void noteOn(double frequency, float velocity, std::int64_t releaseAt) noexcept;

    /// Adds every voice into `out[ch][startFrame .. startFrame + numFrames)`
    /// with `settings`. Channels after the second get nothing; a mono output
    /// gets the signal once.
    void render(const SynthSettings& settings, float* const* out, int numChannels, int startFrame,
        int numFrames) noexcept;

    /// Frequency factor of every voice, live; clamped to 1/16 .. 16.
    void setPitch(double ratio) noexcept;

    /// The smallest releaseAt among the notes still held; kNoRelease if none.
    [[nodiscard]] std::int64_t nextRelease() const noexcept;
    /// Releases the held notes whose releaseAt is at or before `upTo`.
    void release(std::int64_t upTo) noexcept;
    /// Releases every held note.
    void releaseHeld() noexcept;
    /// Fades out every voice over kFadeFrames.
    void stopAll() noexcept;

    /// Voices still sounding: held, releasing or fading.
    [[nodiscard]] int activeVoices() const noexcept;

private:
    enum class Stage : std::uint8_t { Off, Attack, Decay, Sustain, Release };

    struct Voice {
        Stage stage = Stage::Off;
        double phase = 0.0; ///< 0 .. 1
        double frequency = 0.0; ///< Hz, before the pitch factor
        float velocity = 1.0f;
        float level = 0.0f; ///< envelope
        float releaseStep = 0.0f; ///< per frame once released; 0 = not worked out yet
        std::int64_t releaseAt = kNoRelease;
        bool fading = false; ///< stolen or stopped: kFadeFrames to silence
        int fadeLeft = 0;
        float low = 0.0f, band = 0.0f; ///< filter state
        std::uint64_t age = 0;
    };

    static constexpr int kFadeSlots = 8;

    int maxVoices_;
    double sampleRate_ = 48000.0;
    double pitch_ = 1.0;
    std::vector<Voice> voices_; ///< maxVoices_ + kFadeSlots, sized once
    std::uint64_t notes_ = 0;
};

} // namespace vloom
