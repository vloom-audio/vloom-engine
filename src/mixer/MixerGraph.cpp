// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/mixer/MixerGraph.hpp"

#include <algorithm>
#include <cstdint>

namespace vloom {

namespace {

const InsertState kUnity{};

using Edges = std::array<std::vector<int>, MixerGraph::kNodes>;

bool validTarget(int target) noexcept { return target >= 0 && target <= kMaxInserts; }

/// True when `from` reaches `to` over `edges` (targets per node).
bool reaches(const Edges& edges, int from, int to)
{
    std::array<bool, MixerGraph::kNodes> seen{};
    std::vector<int> stack{from};
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        if (node == to)
            return true;
        if (seen[static_cast<std::size_t>(node)])
            continue;
        seen[static_cast<std::size_t>(node)] = true;
        for (const int next : edges[static_cast<std::size_t>(node)])
            stack.push_back(next);
    }
    return false;
}

/// The route of `previous` in the same slot of the same insert, if it goes
/// to the same place the same way.
const MixerRoute* sameRoute(const MixerGraph* previous, const MixerRoute& route) noexcept
{
    if (previous == nullptr)
        return nullptr;
    const auto source = static_cast<std::size_t>(route.source);
    const int first = previous->firstRoute[source];
    for (int i = first; i < first + previous->routeCount[source]; ++i) {
        const MixerRoute& old = previous->routes[static_cast<std::size_t>(i)];
        if (old.slot == route.slot)
            return old.target == route.target && old.preFader == route.preFader ? &old : nullptr;
    }
    return nullptr;
}

/// The latency of an insert's effects, in frames.
int chainLatency(const InsertState& insert)
{
    std::int64_t total = 0;
    for (const auto& effect : insert.effects)
        if (effect != nullptr)
            total += effect->latencyFrames();
    return static_cast<int>(std::clamp<std::int64_t>(total, 0, kMaxLatencyFrames));
}

} // namespace

const InsertState& MixerGraph::insert(int insert) const noexcept
{
    if (state == nullptr || insert < 1 || static_cast<std::size_t>(insert) > state->inserts.size())
        return kUnity;
    return state->inserts[static_cast<std::size_t>(insert) - 1];
}

std::unique_ptr<MixerGraph> buildMixerGraph(
    std::unique_ptr<const MixerState> state, const MixerGraph* previous)
{
    auto graph = std::make_unique<MixerGraph>();
    graph->state = std::move(state);

    // Routes in insert order, each insert's output first; one that would
    // close a loop is left out.
    Edges edges;
    std::array<std::vector<MixerRoute>, MixerGraph::kNodes> bySource;
    const auto add = [&](const MixerRoute& route) {
        if (!validTarget(route.target) || route.target == route.source
            || (route.target != 0 && reaches(edges, route.target, route.source)))
            return;
        edges[static_cast<std::size_t>(route.source)].push_back(route.target);
        bySource[static_cast<std::size_t>(route.source)].push_back(route);
    };
    for (int i = 1; i <= kMaxInserts; ++i) {
        const InsertState& insert = graph->insert(i);
        MixerRoute out;
        out.source = i;
        out.target = insert.output;
        add(out);
        const auto sends = std::min(insert.sends.size(), static_cast<std::size_t>(kMaxSends));
        for (std::size_t s = 0; s < sends; ++s) {
            MixerRoute send;
            send.source = i;
            send.target = insert.sends[s].target;
            send.gain = insert.sends[s].gain;
            send.preFader = insert.sends[s].preFader;
            send.slot = static_cast<int>(s) + 1;
            add(send);
        }
    }

    // Every insert after all those that play into it (Kahn, lowest first).
    std::array<int, MixerGraph::kNodes> incoming{};
    for (int i = 1; i <= kMaxInserts; ++i)
        for (const int target : edges[static_cast<std::size_t>(i)])
            ++incoming[static_cast<std::size_t>(target)];
    graph->order.reserve(static_cast<std::size_t>(kMaxInserts));
    std::array<bool, MixerGraph::kNodes> placed{};
    while (graph->order.size() < static_cast<std::size_t>(kMaxInserts)) {
        int next = 0;
        for (int i = 1; i <= kMaxInserts && next == 0; ++i)
            if (!placed[static_cast<std::size_t>(i)] && incoming[static_cast<std::size_t>(i)] == 0)
                next = i;
        if (next == 0)
            break; // cannot happen: loops were left out
        placed[static_cast<std::size_t>(next)] = true;
        graph->order.push_back(next);
        for (const int target : edges[static_cast<std::size_t>(next)])
            --incoming[static_cast<std::size_t>(target)];
    }

    for (int i = 1; i <= kMaxInserts; ++i) {
        const auto node = static_cast<std::size_t>(i);
        graph->firstRoute[node] = static_cast<int>(graph->routes.size());
        graph->routeCount[node] = static_cast<int>(bySource[node].size());
        for (const MixerRoute& route : bySource[node])
            graph->routes.push_back(route);
    }
    graph->compensate(previous);
    return graph;
}

void MixerGraph::compensate(const MixerGraph* previous)
{
    // When each node's sound is ready: when its input is lined up
    // (`arrive`), plus its effects' latency.
    std::array<int, kNodes> arrive{};
    std::array<int, kNodes> ready{};
    for (const int i : order) {
        const auto node = static_cast<std::size_t>(i);
        ready[node] = std::min(arrive[node] + chainLatency(insert(i)), kMaxLatencyFrames);
        for (int r = firstRoute[node]; r < firstRoute[node] + routeCount[node]; ++r) {
            const auto target = static_cast<std::size_t>(routes[static_cast<std::size_t>(r)].target);
            arrive[target] = std::max(arrive[target], ready[node]);
        }
    }
    inputDelay = arrive;
    latency = arrive[0];

    for (MixerRoute& route : routes) {
        route.delay = std::clamp(
            arrive[static_cast<std::size_t>(route.target)] - ready[static_cast<std::size_t>(route.source)], 0,
            kMaxLatencyFrames);
        const MixerRoute* old = sameRoute(previous, route);
        if (old != nullptr && old->state != nullptr && old->state->line.maxDelay() >= route.delay) {
            route.state = old->state;
        } else if (route.state == nullptr || route.state->line.maxDelay() < route.delay) {
            auto fresh = std::make_shared<MixerRouteState>();
            fresh->line.resize(route.delay);
            if (route.state != nullptr) { // keeps the ramp going
                fresh->applied = route.state->applied;
                fresh->started = route.state->started;
            }
            route.state = std::move(fresh);
        }
    }
    for (std::size_t node = 0; node < kNodes; ++node) {
        const int delay = inputDelay[node];
        if (delay == 0) {
            inputLine[node] = nullptr;
            continue;
        }
        if (previous != nullptr && previous->inputLine[node] != nullptr
            && previous->inputLine[node]->maxDelay() >= delay)
            inputLine[node] = previous->inputLine[node];
        else if (inputLine[node] == nullptr || inputLine[node]->maxDelay() < delay)
            inputLine[node] = std::make_shared<DelayLine>(delay);
    }
}

} // namespace vloom
