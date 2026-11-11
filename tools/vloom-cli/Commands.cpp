// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "Commands.hpp"

#include <vloom/engine.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace vloom::cli {

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
    engine.setTempo(args.bpm);
    engine.play();

    if (!device.start(&error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const double seconds = args.seconds > 0.0 ? args.seconds : 8.0;
    std::printf("Playing metronome on '%s' at %d Hz, %.1f BPM for %.1f s\n", device.deviceName().c_str(),
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
    engine.setTempo(args.bpm);
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
