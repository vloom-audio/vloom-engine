// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// A plugin whose main bus is wider than stereo (5.1, 7.1 ...) in a mixer
// that is stereo (ADR 0014): where the host's left and right go in, and how
// the plugin's speakers are folded down to left and right on the way out.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vloom::speakers {

enum class Speaker : std::uint8_t {
    Left,
    Right,
    Centre,
    Lfe,
    LeftSurround, ///< behind, left
    RightSurround,
    LeftOfCentre,
    RightOfCentre,
    BackCentre,
    LeftSide,
    RightSide,
    TopCentre,
    TopFrontLeft,
    TopFrontCentre,
    TopFrontRight,
    TopBackLeft,
    TopBackCentre,
    TopBackRight,
    Unknown,
};

inline constexpr int kMaxChannels = 32;

/// How much of a speaker goes to the left and to the right of a stereo
/// pair: the usual fold-down (ITU-R BS.775). The centre goes to both at
/// -3 dB, what is on one side to that side at -3 dB, the bass channel is
/// left out, and a speaker the host cannot place is left out too.
struct Gains {
    float left = 0.0f;
    float right = 0.0f;
};

[[nodiscard]] constexpr Gains fold(Speaker speaker) noexcept
{
    constexpr float kHalfPower = 0.70710678f;
    switch (speaker) {
    case Speaker::Left:
        return {1.0f, 0.0f};
    case Speaker::Right:
        return {0.0f, 1.0f};
    case Speaker::Centre:
        return {kHalfPower, kHalfPower};
    case Speaker::LeftSurround:
    case Speaker::LeftSide:
    case Speaker::TopFrontLeft:
    case Speaker::TopBackLeft:
        return {kHalfPower, 0.0f};
    case Speaker::RightSurround:
    case Speaker::RightSide:
    case Speaker::TopFrontRight:
    case Speaker::TopBackRight:
        return {0.0f, kHalfPower};
    case Speaker::LeftOfCentre:
        return {0.85355339f, 0.35355339f};
    case Speaker::RightOfCentre:
        return {0.35355339f, 0.85355339f};
    case Speaker::BackCentre:
    case Speaker::TopCentre:
    case Speaker::TopFrontCentre:
    case Speaker::TopBackCentre:
        return {0.5f, 0.5f};
    case Speaker::Lfe:
    case Speaker::Unknown:
        break;
    }
    return {};
}

/// One bus as the host uses it.
struct Layout {
    int channels = 0;
    int left = 0; ///< the channel the host's left goes into
    int right = 1; ///< and its right
    std::array<Gains, kMaxChannels> gains{}; ///< per channel: its share of the stereo pair
    /// True for a bus of several speakers the host folds down: more than
    /// two channels, a left and a right among them. Anything else is used
    /// as before: one channel as mono, the first two as left and right.
    bool wide = false;
};

/// The layout of a bus with these speakers, in channel order.
[[nodiscard]] inline Layout layoutOf(std::span<const Speaker> bus) noexcept
{
    Layout layout;
    layout.channels
        = static_cast<int>(bus.size() < static_cast<std::size_t>(kMaxChannels) ? bus.size() : kMaxChannels);
    int left = -1;
    int right = -1;
    for (int ch = 0; ch < layout.channels; ++ch) {
        const Speaker speaker = bus[static_cast<std::size_t>(ch)];
        layout.gains[static_cast<std::size_t>(ch)] = fold(speaker);
        if (speaker == Speaker::Left && left < 0)
            left = ch;
        if (speaker == Speaker::Right && right < 0)
            right = ch;
    }
    layout.wide = layout.channels > 2 && left >= 0 && right >= 0;
    if (layout.wide) {
        layout.left = left;
        layout.right = right;
    }
    return layout;
}

/// The layout a bus of `channels` usually has when nothing says which
/// speakers they are: 3 L R C, 4 quad, 5 and 6 as 5.0 and 5.1 (L R C LFE Ls
/// Rs), 7 and 8 as 7.0 and 7.1 (then the sides). Other counts are not
/// guessed: the first two channels are used.
[[nodiscard]] inline Layout layoutOfCount(int channels) noexcept
{
    using enum Speaker;
    static constexpr std::array<Speaker, 3> k30{Left, Right, Centre};
    static constexpr std::array<Speaker, 4> k40{Left, Right, LeftSurround, RightSurround};
    static constexpr std::array<Speaker, 5> k50{Left, Right, Centre, LeftSurround, RightSurround};
    static constexpr std::array<Speaker, 6> k51{Left, Right, Centre, Lfe, LeftSurround, RightSurround};
    static constexpr std::array<Speaker, 7> k70{
        Left, Right, Centre, LeftSurround, RightSurround, LeftSide, RightSide};
    static constexpr std::array<Speaker, 8> k71{
        Left, Right, Centre, Lfe, LeftSurround, RightSurround, LeftSide, RightSide};
    switch (channels) {
    case 3:
        return layoutOf(k30);
    case 4:
        return layoutOf(k40);
    case 5:
        return layoutOf(k50);
    case 6:
        return layoutOf(k51);
    case 7:
        return layoutOf(k70);
    case 8:
        return layoutOf(k71);
    default:
        break;
    }
    Layout layout;
    layout.channels = channels;
    return layout;
}

/// Folds one frame of a wide bus down: `channel(ch)` gives channel ch's sample.
template <typename Channel>
[[nodiscard]] inline Gains folded(const Layout& layout, Channel&& channel) noexcept
{
    Gains sum;
    for (int ch = 0; ch < layout.channels; ++ch) {
        const Gains share = layout.gains[static_cast<std::size_t>(ch)];
        const float sample = channel(ch);
        sum.left += share.left * sample;
        sum.right += share.right * sample;
    }
    return sum;
}

} // namespace vloom::speakers
