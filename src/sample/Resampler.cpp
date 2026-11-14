// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sample/Resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <system_error>
#include <thread>
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

constexpr unsigned kMaxThreads = 16;
constexpr std::int64_t kMinFramesPerThread = 1 << 16; ///< shorter work is not worth a thread

/// What every output frame needs to be computed.
struct Filter {
    const std::vector<double>* table = nullptr;
    double ratio = 1.0; ///< output rate / input rate
    double cutoff = 1.0; ///< relative to the input's Nyquist frequency
    double halfWidth = 0.0; ///< in input samples
    std::int64_t last = 0; ///< the input's last frame
};

/// Output frames [from, to).
struct Range {
    std::int64_t from = 0;
    std::int64_t to = 0;
};

double kernel(const std::vector<double>& table, double x)
{
    const double pos = std::abs(x) * kTableSteps;
    const auto index = static_cast<std::size_t>(pos);
    if (index + 1 >= table.size())
        return 0.0;
    const double frac = pos - static_cast<double>(index);
    return table[index] + (table[index + 1] - table[index]) * frac;
}

/// Computes output frames `range` of every channel, using `weights` (room
/// for every tap) as scratch. Allocates nothing, so it may run on a worker
/// thread.
void resampleRange(
    const SampleBuffer& input, SampleBuffer& output, const Filter& f, Range range, double* weights) noexcept
{
    // The filter taps of an output frame are the same for every channel:
    // worked out once per frame, then applied to each channel (the same sums
    // in the same order as per channel).
    for (std::int64_t n = range.from; n < range.to; ++n) {
        const double t = static_cast<double>(n) / f.ratio; // position in input samples
        const auto first = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::ceil(t - f.halfWidth)));
        const auto end
            = std::min<std::int64_t>(f.last, static_cast<std::int64_t>(std::floor(t + f.halfWidth)));
        if (end < first) {
            for (int ch = 0; ch < input.channels(); ++ch)
                output.channel(ch)[n] = 0.0f;
            continue;
        }
        const auto taps = static_cast<std::size_t>(end - first + 1);
        for (std::size_t i = 0; i < taps; ++i)
            weights[i] = kernel(
                *f.table, (t - static_cast<double>(first + static_cast<std::int64_t>(i))) * f.cutoff);
        for (int ch = 0; ch < input.channels(); ++ch) {
            const float* in = input.channel(ch) + first;
            double sum = 0.0;
            for (std::size_t i = 0; i < taps; ++i)
                sum += static_cast<double>(in[i]) * weights[i];
            output.channel(ch)[n] = static_cast<float>(sum * f.cutoff);
        }
    }
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

    // Output frames are independent of each other: long inputs are split
    // into ranges worked on by several threads at once. Each frame is
    // computed the same way whichever thread does it, so the result does
    // not depend on the thread count.
    const Range all{0, outFrames};
    const Filter filter{&table, ratio, cutoff, halfWidth, last};
    unsigned threads = std::thread::hardware_concurrency();
    threads = std::clamp(threads, 1u, kMaxThreads);
    const auto perThread = std::max<std::int64_t>(kMinFramesPerThread, (outFrames + threads - 1) / threads);
    const auto ranges
        = static_cast<unsigned>(std::min<std::int64_t>(threads, (outFrames + perThread - 1) / perThread));
    // Scratch for each range's taps, allocated here so the workers allocate nothing.
    const auto maxTaps = static_cast<std::size_t>(2.0 * halfWidth) + 2;
    std::vector<std::vector<double>> scratch(std::max(ranges, 1u), std::vector<double>(maxTaps));
    if (ranges <= 1) {
        resampleRange(input, output, filter, all, scratch[0].data());
        return output;
    }
    std::vector<std::thread> workers;
    workers.reserve(ranges - 1);
    std::int64_t from = 0;
    for (unsigned r = 0; r + 1 < ranges; ++r, from += perThread) {
        const Range range{from, from + perThread};
        double* weights = scratch[r].data();
        try {
            workers.emplace_back([&input, &output, &filter, range, weights] {
                resampleRange(input, output, filter, range, weights);
            });
        } catch (const std::system_error&) {
            resampleRange(input, output, filter, range, weights); // no thread to be had: here, then
        }
    }
    // The last range on this thread.
    resampleRange(input, output, filter, Range{from, outFrames}, scratch[ranges - 1].data());
    for (std::thread& worker : workers)
        worker.join();
    return output;
}

} // namespace vloom
