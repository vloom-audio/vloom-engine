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
    PlayOptions options;
    options.gain = gain;
    options.pan = pan;
    options.startFrame = startFrame;
    play(sample, options);
}

void Sampler::startFade(Voice& voice, int frames) noexcept
{
    if (voice.fading && voice.fadeLeft <= frames)
        return; // already ending sooner
    int length = frames;
    if (voice.fading) {
        // A shorter fade (a stolen voice) takes over a release: it starts
        // from the release's current level so it does not jump.
        const double level = static_cast<double>(voice.fadeLeft) / static_cast<double>(voice.fadeLength + 1);
        length = std::max(frames, static_cast<int>(std::lround(static_cast<double>(frames) / level)) - 1);
    }
    voice.fading = true;
    voice.fadeLeft = frames;
    voice.fadeLength = length;
}

void Sampler::setPitch(double ratio) noexcept
{
    if (std::isfinite(ratio))
        pitch_ = std::clamp(ratio, kMinRate, kMaxRate);
}

std::int64_t Sampler::nextRelease() const noexcept
{
    std::int64_t next = kNoRelease;
    for (const Voice& v : voices_)
        if (v.sample != nullptr && !v.fading)
            next = std::min(next, v.releaseAt);
    return next;
}

void Sampler::release(std::int64_t upTo) noexcept
{
    for (Voice& v : voices_)
        if (v.sample != nullptr && !v.fading && v.releaseAt != kNoRelease && v.releaseAt <= upTo)
            startFade(v, kReleaseFrames);
}

void Sampler::releaseHeld() noexcept { release(kNoRelease - 1); }

void Sampler::play(const SampleBuffer* sample, const PlayOptions& options) noexcept
{
    const std::int64_t startFrame = options.startFrame;
    if (sample == nullptr || sample->empty() || !std::isfinite(options.gain) || !std::isfinite(options.pan)
        || !std::isfinite(options.rate) || startFrame < 0 || startFrame >= sample->frames())
        return;
    const float pan = std::clamp(options.pan, -1.0f, 1.0f);
    const float gain = std::max(options.gain, 0.0f);

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
    if (sounding >= maxVoices_ && oldest != nullptr)
        startFade(*oldest, kFadeFrames);

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
    voice.rate = std::clamp(options.rate, kMinRate, kMaxRate);
    voice.followsPitch = options.followsPitch;
    voice.releaseAt = options.releaseAt;
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

    const std::int64_t length = s.frames();
    int limit = numFrames;
    if (v.fading)
        limit = std::min(limit, v.fadeLeft);

    const auto fadeAt = [&v](int i) {
        // Linear fade from just under 1 down to just over 0.
        float fade
            = v.fading ? static_cast<float>(v.fadeLeft - i) / static_cast<float>(v.fadeLength + 1) : 1.0f;
        if (i < v.fadeInLeft) // rising from just over 0
            fade *= static_cast<float>(kFadeFrames - v.fadeInLeft + i + 1)
                / static_cast<float>(kFadeFrames + 1);
        return fade;
    };
    const auto add = [&](int i, float l, float r, float fade) {
        if (outRight == nullptr) {
            // Mono output: sum both sides.
            outLeft[i] += 0.5f * (l * v.gainLeft + r * v.gainRight) * fade;
        } else {
            outLeft[i] += l * v.gainLeft * fade;
            outRight[i] += r * v.gainRight * fade;
        }
    };

    const double speed = v.rate * (v.followsPitch ? pitch_ : 1.0);
    int frames = 0;
    if (speed == 1.0 && v.fraction == 0.0) {
        // Frame for frame: the sample exactly as it is.
        frames = static_cast<int>(std::min<std::int64_t>(limit, length - v.position));
        for (int i = 0; i < frames; ++i) {
            const std::int64_t at = v.position + i;
            add(i, left[at], right[at], fadeAt(i));
        }
        v.position += frames;
    } else {
        // 4-point, 3rd-order Hermite; frames outside the sample read as silence.
        const auto read = [length](const float* data, std::int64_t at) {
            return at >= 0 && at < length ? data[at] : 0.0f;
        };
        const auto hermite = [&read](const float* data, std::int64_t at, float t) {
            const float xm1 = read(data, at - 1);
            const float x0 = data[at];
            const float x1 = read(data, at + 1);
            const float x2 = read(data, at + 2);
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * t + c2) * t + c1) * t + x0;
        };
        for (; frames < limit && v.position < length; ++frames) {
            const auto t = static_cast<float>(v.fraction);
            const float l = hermite(left, v.position, t);
            const float r = right == left ? l : hermite(right, v.position, t);
            add(frames, l, r, fadeAt(frames));
            v.fraction += speed;
            const double whole = std::floor(v.fraction);
            v.position += static_cast<std::int64_t>(whole);
            v.fraction -= whole;
        }
    }

    v.fadeInLeft = std::max(0, v.fadeInLeft - frames);
    if (v.fading)
        v.fadeLeft -= frames;
    if (v.position >= s.frames() || (v.fading && v.fadeLeft <= 0))
        v = Voice{}; // finished: the slot is free again
}

void Sampler::stopAll() noexcept
{
    for (Voice& v : voices_)
        if (v.sample != nullptr)
            startFade(v, kFadeFrames); // a release in progress ends sooner too
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
