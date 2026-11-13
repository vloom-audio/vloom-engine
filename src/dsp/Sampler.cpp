// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/dsp/Sampler.hpp"

#include "vloom/engine/sample/SampleBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vloom {

Sampler::Sampler(int maxVoices)
    : maxVoices_(std::clamp(maxVoices, 1, kMaxPolyphony))
    , voices_(static_cast<std::size_t>(maxVoices_ + kFadeSlots))
{
}

void Sampler::reset() noexcept
{
    std::fill(voices_.begin(), voices_.end(), Voice{});
    triggers_ = 0;
}

void Sampler::trigger(const SampleBuffer* sample, float gain, float pan, std::int64_t startFrame) noexcept
{
    if (sample == nullptr || sample->empty() || !std::isfinite(gain) || !std::isfinite(pan) || startFrame < 0
        || startFrame >= sample->frames())
        return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    gain = std::max(gain, 0.0f);

    // Keep at most maxVoices_ sounding: the oldest one fades out.
    int sounding = 0;
    Voice* oldest = nullptr;
    for (Voice& v : voices_) {
        if (v.sample == nullptr || v.fading)
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
        if (v.sample == nullptr) {
            slot = &v;
            break;
        }
        if (v.fading && (slot == nullptr || v.fadeLeft < slot->fadeLeft))
            slot = &v;
    }
    if (slot == nullptr)
        return; // unreachable: there are more slots than voices

    Voice voice;
    voice.sample = sample;
    voice.age = ++triggers_;
    voice.position = startFrame;
    voice.fadeInLeft = startFrame > 0 ? kFadeFrames : 0;
    if (sample->channels() == 1) {
        // Constant power: -3 dB per side in the centre.
        const double angle = (static_cast<double>(pan) + 1.0) * std::numbers::pi / 4.0;
        voice.gainLeft = gain * static_cast<float>(std::cos(angle));
        voice.gainRight = gain * static_cast<float>(std::sin(angle));
    } else {
        // Balance: the side panned away from is turned down.
        voice.gainLeft = gain * std::min(1.0f, 1.0f - pan);
        voice.gainRight = gain * std::min(1.0f, 1.0f + pan);
    }
    *slot = voice;
}

void Sampler::render(float* const* out, int numChannels, int startFrame, int numFrames) noexcept
{
    if (numFrames <= 0 || numChannels <= 0)
        return;
    for (Voice& v : voices_)
        if (v.sample != nullptr)
            renderVoice(v, out, numChannels, startFrame, numFrames);
}

void Sampler::renderVoice(
    Voice& v, float* const* out, int numChannels, int startFrame, int numFrames) noexcept
{
    const SampleBuffer& s = *v.sample;
    const float* left = s.channel(0);
    const float* right = s.channels() > 1 ? s.channel(1) : left;
    float* outLeft = out[0] + startFrame;
    float* outRight = numChannels > 1 ? out[1] + startFrame : nullptr;

    const std::int64_t remaining = s.frames() - v.position;
    int frames = static_cast<int>(std::min<std::int64_t>(numFrames, remaining));
    if (v.fading)
        frames = std::min(frames, v.fadeLeft);

    for (int i = 0; i < frames; ++i) {
        const std::int64_t at = v.position + i;
        // Linear fade from just under 1 down to just over 0.
        float fade
            = v.fading ? static_cast<float>(v.fadeLeft - i) / static_cast<float>(kFadeFrames + 1) : 1.0f;
        if (i < v.fadeInLeft) // rising from just over 0
            fade *= static_cast<float>(kFadeFrames - v.fadeInLeft + i + 1)
                / static_cast<float>(kFadeFrames + 1);
        if (outRight == nullptr) {
            // Mono output: sum both sides.
            outLeft[i] += 0.5f * (left[at] * v.gainLeft + right[at] * v.gainRight) * fade;
        } else {
            outLeft[i] += left[at] * v.gainLeft * fade;
            outRight[i] += right[at] * v.gainRight * fade;
        }
    }

    v.position += frames;
    v.fadeInLeft = std::max(0, v.fadeInLeft - frames);
    if (v.fading)
        v.fadeLeft -= frames;
    if (v.position >= s.frames() || (v.fading && v.fadeLeft <= 0))
        v = Voice{}; // finished: the slot is free again
}

void Sampler::stopAll() noexcept
{
    for (Voice& v : voices_)
        if (v.sample != nullptr && !v.fading) {
            v.fading = true;
            v.fadeLeft = kFadeFrames;
        }
}

bool Sampler::uses(const SampleBuffer* sample) const noexcept
{
    return sample != nullptr && std::any_of(voices_.begin(), voices_.end(), [sample](const Voice& v) {
        return v.sample == sample;
    });
}

int Sampler::activeVoices() const noexcept
{
    return static_cast<int>(
        std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.sample != nullptr; }));
}

} // namespace vloom
