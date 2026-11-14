// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/dsp/DelayLine.hpp"
#include "vloom/engine/mixer/MixerState.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace vloom {

/// What the audio thread keeps of one route between blocks: the gain it
/// last applied (for the ramp) and the route's compensation delay. Shared
/// by the graphs that keep the route, so an edit does not cut its sound.
struct MixerRouteState {
    DelayLine line;
    float applied = 0.0f;
    bool started = false; ///< applied is set: false until the first block
};

/// One route the mixer plays: an insert's output or one of its sends.
struct MixerRoute {
    int source = 1; ///< insert 1 .. kMaxInserts
    int target = 0; ///< 0 = the master, 1 .. kMaxInserts
    float gain = 1.0f;
    bool preFader = false;
    int slot = 0; ///< 0 = the output, n = send n - 1
    int delay = 0; ///< frames of compensation (ADR 0007)
    std::shared_ptr<MixerRouteState> state;
};

/// A MixerState compiled for the audio thread (ADR 0007), built on the
/// control thread by Engine::setMixerState(): the inserts in an order that
/// processes every insert before those it plays into, the routes (loops
/// left out) and the delays that line up paths with plugin latency.
struct MixerGraph {
    static constexpr std::size_t kNodes = static_cast<std::size_t>(kMaxInserts) + 1; ///< 0 = the master
    /// Most delay-line frames one graph holds, all lines together (stereo:
    /// 64 MiB). Paths that would need more are left uncompensated.
    static constexpr std::int64_t kMaxCompensationFrames = std::int64_t{1} << 23;

    std::unique_ptr<const MixerState> state; ///< may be null: every insert at unity into the master
    std::vector<int> order; ///< inserts 1 .. kMaxInserts, sources before targets
    std::vector<MixerRoute> routes; ///< grouped by source, in slot order
    std::array<int, kNodes> firstRoute{}; ///< per insert: its first route in `routes`
    std::array<int, kNodes> routeCount{};

    // Latency: worked out from the effects' latency by compensate(), again
    // by Engine::prepare() (while audio is stopped) when the rate changes.
    /// Per node: frames its direct input (channels, the preview) is delayed
    /// by, to line up with what arrives from inserts with latency.
    std::array<int, kNodes> inputDelay{};
    std::array<std::shared_ptr<DelayLine>, kNodes> inputLine{};
    /// Frames the master's sound comes late, compared with the transport.
    int latency = 0;

    /// The insert `insert` (1 .. kMaxInserts) of the state, or unity.
    [[nodiscard]] const InsertState& insert(int insert) const noexcept;

    /// Works out the delays from the effects' current latency. Reuses the
    /// delay lines of `previous` (may be null) where they are long enough,
    /// so a ringing path is not cut. Control thread; not while the audio
    /// thread processes this graph.
    void compensate(const MixerGraph* previous);
};

/// Compiles `state` (null: all unity) for the audio thread, reusing what
/// routes kept from `previous` (may be null). Control thread; allocates.
[[nodiscard]] std::unique_ptr<MixerGraph> buildMixerGraph(
    std::unique_ptr<const MixerState> state, const MixerGraph* previous);

} // namespace vloom
