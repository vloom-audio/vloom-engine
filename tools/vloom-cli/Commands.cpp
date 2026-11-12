// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "Commands.hpp"

#include <vloom/engine.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace vloom::cli {

namespace {

/// Builds the channel rack from --channel options, loading every sample at
/// `sampleRate`. Null (with a message printed) on bad input.
std::unique_ptr<const SequencerState> buildRack(const Args& args, double sampleRate)
{
    auto state = std::make_unique<SequencerState>();
    int longest = 0;
    for (const ChannelArg& arg : args.channels) {
        ChannelState channel;
        for (const char c : arg.pattern) {
            if (c == 'x' || c == 'X')
                channel.steps.push_back(1);
            else if (c == '.' || c == '-')
                channel.steps.push_back(0);
            else if (c != '|') {
                std::fprintf(stderr, "error: bad step '%c' in pattern %s\n", c, arg.pattern.c_str());
                return nullptr;
            }
        }
        std::string error;
        channel.sample = loadSample(arg.path, sampleRate, &error);
        if (!channel.sample) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return nullptr;
        }
        channel.voices = std::make_shared<ChannelVoices>();
        longest = std::max(longest, static_cast<int>(channel.steps.size()));
        state->channels.push_back(std::move(channel));
    }
    state->stepCount = std::max(longest, 1);
    return state;
}

/// Sets tempo, metronome and channel rack from the arguments.
bool setUp(Engine& engine, const Args& args, double sampleRate)
{
    engine.setTempo(args.bpm);
    engine.setMetronomeEnabled(args.click.value_or(args.channels.empty()));
    if (args.channels.empty())
        return true;
    auto rack = buildRack(args, sampleRate);
    if (!rack)
        return false;
    engine.setSequencerState(std::move(rack));
    return true;
}

} // namespace

int listDevices()
{
    AudioDevice device;
    const auto devices = device.listOutputDevices();
    if (devices.empty()) {
        std::puts("No output devices found.");
        return 1;
    }
    for (std::size_t i = 0; i < devices.size(); ++i)
        std::printf("%2zu  %s%s\n", i, devices[i].name.c_str(), devices[i].isDefault ? "  (default)" : "");
    return 0;
}

int play(const Args& args)
{
    Engine engine;
    AudioDevice device;

    AudioDeviceConfig cfg;
    cfg.deviceIndex = args.device;
    cfg.bufferFrames = args.buffer;

    std::string error;
    auto callback = [&engine](float* const* out, int ch, int n) { engine.process(out, ch, n); };
    if (!device.open(cfg, callback, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    engine.prepare(device.sampleRate(), AudioDevice::kMaxBlockFrames);
    if (!setUp(engine, args, device.sampleRate()))
        return 1;
    engine.play();

    if (!device.start(&error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const double seconds = args.seconds > 0.0 ? args.seconds : 8.0;
    std::printf("Playing %s on '%s' at %d Hz, %.1f BPM for %.1f s\n",
        args.channels.empty() ? "metronome" : "channel rack", device.deviceName().c_str(),
        device.sampleRate(), args.bpm, seconds);

    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        const auto bbt = toBarBeatTick(engine.positionTicks(), engine.beatsPerBar());
        std::printf("\r  %3lld.%d.%03d", static_cast<long long>(bbt.bar), bbt.beat, bbt.tick);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::puts("");

    device.stop();
    return 0;
}

int render(const Args& args)
{
    if (args.output.empty()) {
        std::fputs("error: render needs an output file\n", stderr);
        return 1;
    }

    Engine engine;
    engine.prepare(args.rate, 512);
    if (!setUp(engine, args, args.rate))
        return 1;
    engine.play();

    OfflineRenderOptions opts;
    opts.seconds = args.seconds > 0.0 ? args.seconds : 4.0;

    std::string error;
    if (!renderToWav(engine, args.output, opts, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("Wrote %.1f s at %d Hz to %s\n", opts.seconds, args.rate, args.output.c_str());
    return 0;
}

} // namespace vloom::cli
