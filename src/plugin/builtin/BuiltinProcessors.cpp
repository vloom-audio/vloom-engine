// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/BuiltinProcessors.hpp"

#include "plugin/builtin/BuiltinEffect.hpp"

#include <vloom/dsp/effects/Gain.hpp>

#include <array>

namespace vloom {

namespace {

struct Entry {
    PluginInfo (*info)();
    std::unique_ptr<AudioProcessor> (*create)();
};

template <dsp::Effect Effect> constexpr Entry entry()
{
    return Entry{[] { return builtin::BuiltinEffect<Effect>().info(); },
        []() -> std::unique_ptr<AudioProcessor> {
            return std::make_unique<builtin::BuiltinEffect<Effect>>();
        }};
}

// One line per built-in. Ids are stable: saved projects refer to them.
constexpr std::array kBuiltins{
    entry<dsp::Gain>(),
};

} // namespace

std::vector<PluginInfo> builtinProcessors()
{
    std::vector<PluginInfo> list;
    list.reserve(kBuiltins.size());
    for (const Entry& builtin : kBuiltins)
        list.push_back(builtin.info());
    return list;
}

std::unique_ptr<AudioProcessor> createBuiltinProcessor(std::string_view id)
{
    for (const Entry& builtin : kBuiltins)
        if (builtin.info().id == id)
            return builtin.create();
    return nullptr;
}

} // namespace vloom
