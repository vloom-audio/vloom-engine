// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/dsp/Synth.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vloom {

namespace {

constexpr float kCentre = 0.70710678f; // -3 dB, like a mono sample panned centre
constexpr double kMinPitch = 1.0 / 16.0;
constexpr double kMaxPitch = 16.0;

/// PolyBLEP residual: smooths the jump of a saw or square at phase 0 over
/// one sample on each side, which removes most of the aliasing.
double polyBlep(double t, double dt) noexcept
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

double oscillator(Waveform waveform, double phase, double dt) noexcept
{
    switch (waveform) {
    case Waveform::Sine:
        return std::sin(2.0 * std::numbers::pi * phase);
    case Waveform::Triangle:
        return 4.0 * std::abs(phase - 0.5) - 1.0;
    case Waveform::Square: {
        double half = phase + 0.5;
        if (half >= 1.0)
            half -= 1.0;
        return (phase < 0.5 ? 1.0 : -1.0) + polyBlep(phase, dt) - polyBlep(half, dt);
    }
    case Waveform::Saw:
        break;
    }
    return 2.0 * phase - 1.0 - polyBlep(phase, dt);
}

float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }

} // namespace

Synth::Synth(int maxVoices)
    : maxVoices_(std::clamp(maxVoices, 1, kMaxPolyphony))
    , voices_(static_cast<std::size_t>(maxVoices_ + kFadeSlots))
{
}

void Synth::setSampleRate(double sampleRate) noexcept
{
    if (std::isfinite(sampleRate) && sampleRate > 0.0)
        sampleRate_ = sampleRate;
}

void Synth::reset() noexcept
{
    std::fill(voices_.begin(), voices_.end(), Voice{});
    notes_ = 0;
}

void Synth::setPitch(double ratio) noexcept
{
    if (std::isfinite(ratio))
        pitch_ = std::clamp(ratio, kMinPitch, kMaxPitch);
}

void Synth::noteOn(double frequency, float velocity, std::int64_t releaseAt) noexcept
{
    if (!std::isfinite(frequency) || frequency <= 0.0 || !std::isfinite(velocity))
        return;

    // Keep at most maxVoices_ sounding: the oldest one fades out.
    int sounding = 0;
    Voice* oldest = nullptr;
    for (Voice& v : voices_) {
        if (v.stage == Stage::Off || v.fading)
            continue;
        ++sounding;
        if (oldest == nullptr || v.age < oldest->age)
            oldest = &v;
    }
    if (sounding >= maxVoices_ && oldest != nullptr) {
        oldest->fading = true;
        oldest->fadeLeft = kFadeFrames;
    }

    // A free slot; if the fade slots are all busy too, cut the fade closest to its end.
    Voice* slot = nullptr;
    for (Voice& v : voices_) {
        if (v.stage == Stage::Off) {
            slot = &v;
            break;
        }
        if (v.fading && (slot == nullptr || v.fadeLeft < slot->fadeLeft))
            slot = &v;
    }
    if (slot == nullptr)
        return; // unreachable: there are more slots than voices

    Voice voice;
    voice.stage = Stage::Attack;
    voice.frequency = frequency;
    voice.velocity = std::clamp(velocity, 0.0f, 1.0f);
    voice.releaseAt = releaseAt;
    voice.age = ++notes_;
    *slot = voice;
}

void Synth::render(
    const SynthSettings& settings, float* const* out, int numChannels, int startFrame, int numFrames) noexcept
{
    if (numFrames <= 0 || numChannels <= 0 || activeVoices() == 0)
        return;

    // Live settings, made safe.
    const SynthSettings defaults;
    const auto rate = static_cast<float>(sampleRate_);
    const float attack = std::clamp(
        finiteOr(settings.attack, defaults.attack), SynthSettings::kMinAttack, SynthSettings::kMaxTime);
    const float decay = std::clamp(
        finiteOr(settings.decay, defaults.decay), SynthSettings::kMinAttack, SynthSettings::kMaxTime);
    const float release = std::clamp(
        finiteOr(settings.release, defaults.release), SynthSettings::kMinRelease, SynthSettings::kMaxTime);
    const float sustain = std::clamp(finiteOr(settings.sustain, defaults.sustain), 0.0f, 1.0f);
    const float resonance = std::clamp(finiteOr(settings.resonance, defaults.resonance), 0.0f, 1.0f);
    const float level = std::clamp(finiteOr(settings.level, defaults.level), 0.0f, 1.0f);
    const float cutoff = std::clamp(finiteOr(settings.cutoff, defaults.cutoff), SynthSettings::kMinCutoff,
        std::min(SynthSettings::kMaxCutoff, 0.45f * rate));

    const float attackStep = 1.0f / (attack * rate);
    const float decayStep = (1.0f - sustain) / (decay * rate);
    const float releaseFrames = release * rate;

    // Topology-preserving state-variable filter (Zavalishin), low-pass output.
    const auto g = static_cast<float>(std::tan(std::numbers::pi * static_cast<double>(cutoff) / sampleRate_));
    const float k = 2.0f - 1.96f * resonance; // damping: 2 = none .. 0.04 = rings
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;

    float* outLeft = out[0] + startFrame;
    float* outRight = numChannels > 1 ? out[1] + startFrame : nullptr;
    const float gain = level * kCentre;

    for (Voice& v : voices_) {
        if (v.stage == Stage::Off)
            continue;
        const double dt = std::min(v.frequency * pitch_ / sampleRate_, 0.49);
        for (int i = 0; i < numFrames; ++i) {
            switch (v.stage) {
            case Stage::Attack:
                v.level += attackStep;
                if (v.level >= 1.0f) {
                    v.level = 1.0f;
                    v.stage = Stage::Decay;
                }
                break;
            case Stage::Decay:
                v.level -= decayStep;
                if (v.level <= sustain) {
                    v.level = sustain;
                    v.stage = Stage::Sustain;
                }
                break;
            case Stage::Sustain:
                v.level = sustain; // follows the setting
                break;
            case Stage::Release:
                if (v.releaseStep == 0.0f) // from wherever the envelope is now
                    v.releaseStep = std::max(v.level, 1e-6f) / releaseFrames;
                v.level -= v.releaseStep;
                break;
            case Stage::Off:
                break;
            }
            if (v.stage == Stage::Release && v.level <= 0.0f) {
                v = Voice{};
                break;
            }

            const auto x = static_cast<float>(oscillator(settings.waveform, v.phase, dt));
            const float v3 = x - v.low;
            const float v1 = a1 * v.band + a2 * v3;
            const float v2 = v.low + a2 * v.band + a3 * v3;
            v.band = 2.0f * v1 - v.band;
            v.low = 2.0f * v2 - v.low;

            float y = v2 * v.level * v.velocity * gain;
            if (v.fading) // linear, from just under 1 down to just over 0
                y *= static_cast<float>(v.fadeLeft) / static_cast<float>(kFadeFrames + 1);
            outLeft[i] += y;
            if (outRight != nullptr)
                outRight[i] += y;

            v.phase += dt;
            if (v.phase >= 1.0)
                v.phase -= 1.0;
            if (v.fading && --v.fadeLeft <= 0) {
                v = Voice{};
                break;
            }
        }
    }
}

std::int64_t Synth::nextRelease() const noexcept
{
    std::int64_t next = kNoRelease;
    for (const Voice& v : voices_)
        if (v.stage != Stage::Off && v.stage != Stage::Release && !v.fading)
            next = std::min(next, v.releaseAt);
    return next;
}

void Synth::release(std::int64_t upTo) noexcept
{
    for (Voice& v : voices_)
        if (v.stage != Stage::Off && v.stage != Stage::Release && !v.fading && v.releaseAt != kNoRelease
            && v.releaseAt <= upTo) {
            v.stage = Stage::Release;
            v.releaseStep = 0.0f;
        }
}

void Synth::releaseHeld() noexcept
{
    for (Voice& v : voices_)
        if (v.stage != Stage::Off && v.stage != Stage::Release && !v.fading) {
            v.stage = Stage::Release;
            v.releaseStep = 0.0f;
        }
}

void Synth::stopAll() noexcept
{
    for (Voice& v : voices_)
        if (v.stage != Stage::Off && !v.fading) {
            v.fading = true;
            v.fadeLeft = kFadeFrames;
        }
}

int Synth::activeVoices() const noexcept
{
    return static_cast<int>(
        std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.stage != Stage::Off; }));
}

} // namespace vloom
