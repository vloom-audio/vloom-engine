// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sample/Resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace vloom {

namespace {

constexpr int kZeroCrossings = 32; ///< filter half-length, in zero crossings of the sinc
constexpr int kTableSteps = 1024; ///< table points per zero crossing
constexpr double kKaiserBeta = 9.0; ///< about 90 dB stop-band attenuation
constexpr double kPassband = 0.97; ///< cutoff as a share of the lower Nyquist frequency

/// Modified Bessel function of the first kind, order 0 (power series).
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double quarterSquare = x * x / 4.0;
    for (int k = 1; k < 50; ++k) {
        term *= quarterSquare / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/// Kaiser-windowed sinc at x zero crossings from the centre, x in [0, kZeroCrossings].
const std::vector<double>& kernelTable()
{
    static const std::vector<double> table = [] {
        std::vector<double> t(static_cast<std::size_t>(kZeroCrossings * kTableSteps + 2), 0.0);
        const double norm = besselI0(kKaiserBeta);
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double x = static_cast<double>(i) / kTableSteps;
            if (x >= kZeroCrossings)
                break;
            const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double r = x / kZeroCrossings;
            t[i] = sinc * besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / norm;
        }
        return t;
    }();
    return table;
}

double kernel(const std::vector<double>& table, double x)
{
    const double pos = std::abs(x) * kTableSteps;
    const auto index = static_cast<std::size_t>(pos);
    if (index + 1 >= table.size())
        return 0.0;
    const double frac = pos - static_cast<double>(index);
    return table[index] + (table[index + 1] - table[index]) * frac;
}

} // namespace

SampleBuffer resample(const SampleBuffer& input, double targetRate)
{
    const double sourceRate = input.sampleRate();
    if (!(targetRate > 0.0) || !(sourceRate > 0.0))
        return {};
    if (targetRate == sourceRate) {
        SampleBuffer copy(input.channels(), input.frames(), sourceRate);
        for (int ch = 0; ch < input.channels(); ++ch)
            std::memcpy(copy.channel(ch), input.channel(ch),
                sizeof(float) * static_cast<std::size_t>(input.frames()));
        return copy;
    }

    const double ratio = targetRate / sourceRate;
    const auto outFrames
        = static_cast<std::int64_t>(std::llround(static_cast<double>(input.frames()) * ratio));
    SampleBuffer output(input.channels(), outFrames, targetRate);

    // Cutoff relative to the input's Nyquist frequency.
    const double cutoff = std::min(1.0, ratio) * kPassband;
    const double halfWidth = kZeroCrossings / cutoff; // in input samples
    const std::vector<double>& table = kernelTable();
    const std::int64_t last = input.frames() - 1;

    for (int ch = 0; ch < input.channels(); ++ch) {
        const float* in = input.channel(ch);
        float* out = output.channel(ch);
        for (std::int64_t n = 0; n < outFrames; ++n) {
            const double t = static_cast<double>(n) / ratio; // position in input samples
            const auto first = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::ceil(t - halfWidth)));
            const auto end
                = std::min<std::int64_t>(last, static_cast<std::int64_t>(std::floor(t + halfWidth)));
            double sum = 0.0;
            for (std::int64_t k = first; k <= end; ++k)
                sum += static_cast<double>(in[k]) * kernel(table, (t - static_cast<double>(k)) * cutoff);
            out[n] = static_cast<float>(sum * cutoff);
        }
    }
    return output;
}

} // namespace vloom
