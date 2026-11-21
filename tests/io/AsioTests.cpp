// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "io/AsioDriver.hpp"
#include "io/AsioSession.hpp"

#include <vloom/engine/io/AudioDevice.hpp>
#include <vloom/engine/io/AudioInput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ASIO (ADR 0021) without a driver: the pure parts, and the session and the
// AudioDevice / AudioInput paths over a fake driver. No real ASIO driver is
// loaded and no device is opened anywhere in these tests.

using namespace vloom::io;

namespace {

/// What the fake driver was set up with and what it saw. Shared with the
/// test, which outlives the driver the session owns.
struct FakeState {
    std::vector<std::string> names{"Fake ASIO", "Other ASIO"};
    int inputs = 2;
    int outputs = 4;
    AsioBufferSizes sizes{64, 2048, 512, -1};
    double rate = 44100.0;
    std::vector<double> rates{44100.0, 48000.0};
    std::int32_t inputType = asio::kInt16Lsb;
    std::int32_t outputType = asio::kInt32Lsb;
    std::int32_t loadResult = asio::kOk;
    std::string loadMessage;
    int inputLatency = 300;
    int outputLatency = 400;
    bool outputReadySupported = true;
    std::function<void(AsioDriverHost&)> duringCreate; ///< called inside createBuffers
    std::function<void()> duringStart; ///< called inside start, with the host connected
    bool throwInCreate = false;
    int stopDelayMs = 0; ///< threadStopping waits this long
    std::shared_ptr<FakeState> previous; ///< the driver of the session before

    // Seen: written on the session's driver thread, read by the test after
    // a control call returned (which orders them).
    std::set<std::thread::id> threads;
    bool comIn = false;
    bool comOut = false;
    bool loaded = false;
    std::string loadedName;
    bool started = false;
    bool buffersCreated = false;
    int setRateCalls = 0;
    int controlPanelCalls = 0;
    int createdFrames = 0;
    std::vector<AsioBuffer> buffers;
    std::vector<std::vector<std::byte>> storage;
    AsioDriverHost* host = nullptr;
    std::atomic<int> outputReadyCalls{0};
    std::atomic<bool> stoppingStarted{false};
    std::atomic<bool> stopped{false};
    bool previousWasStopped = false;
};

int bytesFor(std::int32_t type)
{
    const auto format = sampleFormatForAsioType(type);
    return format ? bytesPerSample(*format) : 4;
}

class FakeAsioDriver final : public AsioDriver {
public:
    explicit FakeAsioDriver(std::shared_ptr<FakeState> state)
        : s_(std::move(state))
    {
    }

    void threadStarted() override
    {
        seen();
        s_->comIn = true;
        if (s_->previous)
            s_->previousWasStopped = s_->previous->stopped.load();
    }
    void threadStopping() override
    {
        seen();
        s_->comOut = true;
        s_->stoppingStarted.store(true);
        if (s_->stopDelayMs > 0) // the old session's teardown, slowed down
            std::this_thread::sleep_for(std::chrono::milliseconds(s_->stopDelayMs));
        s_->stopped.store(true);
    }
    std::vector<std::string> driverNames() override
    {
        seen();
        return s_->names;
    }
    std::int32_t load(const std::string& name, std::string& message) override
    {
        seen();
        if (s_->loadResult != asio::kOk) {
            message = s_->loadMessage;
            return s_->loadResult;
        }
        s_->loaded = true;
        s_->loadedName = name;
        return asio::kOk;
    }
    void unload() noexcept override
    {
        seen();
        s_->loaded = false;
    }
    std::int32_t getChannels(int& inputs, int& outputs) override
    {
        seen();
        inputs = s_->inputs;
        outputs = s_->outputs;
        return asio::kOk;
    }
    std::int32_t getBufferSize(AsioBufferSizes& sizes) override
    {
        seen();
        sizes = s_->sizes;
        return asio::kOk;
    }
    std::int32_t canSampleRate(double rate) override
    {
        seen();
        return std::find(s_->rates.begin(), s_->rates.end(), rate) != s_->rates.end() ? asio::kOk
                                                                                      : asio::kNoClock;
    }
    std::int32_t getSampleRate(double& rate) override
    {
        seen();
        rate = s_->rate;
        return asio::kOk;
    }
    std::int32_t setSampleRate(double rate) override
    {
        seen();
        ++s_->setRateCalls;
        s_->rate = rate;
        return asio::kOk;
    }
    std::int32_t getChannelType(bool isInput, int /*channel*/, std::int32_t& type) override
    {
        seen();
        type = isInput ? s_->inputType : s_->outputType;
        return asio::kOk;
    }
    std::int32_t createBuffers(std::vector<AsioBuffer>& buffers, int frames, AsioDriverHost& host) override
    {
        seen();
        if (s_->throwInCreate)
            throw std::runtime_error("fake driver failure");
        s_->storage.clear();
        s_->storage.reserve(buffers.size() * 2); // the halves' pointers must stay valid
        for (AsioBuffer& buffer : buffers) {
            const int bytes = bytesFor(buffer.isInput ? s_->inputType : s_->outputType) * frames;
            for (void*& half : buffer.halves) {
                // Garbage, so the test sees the session clear the outputs.
                s_->storage.emplace_back(static_cast<std::size_t>(bytes), std::byte{0x5A});
                half = s_->storage.back().data();
            }
        }
        s_->buffers = buffers;
        s_->createdFrames = frames;
        s_->host = &host;
        s_->buffersCreated = true;
        if (s_->duringCreate)
            s_->duringCreate(host);
        return asio::kOk;
    }
    std::int32_t disposeBuffers() override
    {
        seen();
        s_->buffersCreated = false;
        s_->host = nullptr;
        return asio::kOk;
    }
    std::int32_t getLatencies(int& input, int& output) override
    {
        seen();
        input = s_->inputLatency;
        output = s_->outputLatency;
        return asio::kOk;
    }
    std::int32_t start() override
    {
        seen();
        s_->started = true;
        if (s_->duringStart)
            s_->duringStart();
        return asio::kOk;
    }
    std::int32_t stop() override
    {
        seen();
        s_->started = false;
        return asio::kOk;
    }
    std::int32_t controlPanel() override
    {
        seen();
        ++s_->controlPanelCalls;
        return asio::kOk;
    }
    std::int32_t outputReady() noexcept override
    {
        s_->outputReadyCalls.fetch_add(1);
        return s_->outputReadySupported ? asio::kOk : asio::kNotPresent;
    }

private:
    void seen() { s_->threads.insert(std::this_thread::get_id()); }

    std::shared_ptr<FakeState> s_;
};

/// Makes AudioDevice and AudioInput's ASIO use a fake driver while it lives.
class FakeAsioFactory {
public:
    explicit FakeAsioFactory(std::shared_ptr<FakeState> state)
    {
        setAsioDriverFactoryForTesting([state] { return std::make_unique<FakeAsioDriver>(state); });
    }
    ~FakeAsioFactory() { setAsioDriverFactoryForTesting(nullptr); }
    FakeAsioFactory(const FakeAsioFactory&) = delete;
    FakeAsioFactory& operator=(const FakeAsioFactory&) = delete;
};

std::int32_t readInt32(const void* half, std::size_t index)
{
    std::int32_t value = 0;
    std::memcpy(&value, static_cast<const std::byte*>(half) + index * 4, 4);
    return value;
}

void writeInt16(void* half, std::size_t index, std::int16_t value)
{
    std::memcpy(static_cast<std::byte*>(half) + index * 2, &value, 2);
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

// ---- pure parts

TEST_CASE("ASIO sample types map to the engine's formats; MSB and DSD do not", "[io][asio]")
{
    CHECK(sampleFormatForAsioType(asio::kInt16Lsb) == SampleFormat::Int16);
    CHECK(sampleFormatForAsioType(asio::kInt24Lsb) == SampleFormat::Int24Packed);
    CHECK(sampleFormatForAsioType(asio::kInt32Lsb) == SampleFormat::Int32);
    CHECK(sampleFormatForAsioType(asio::kFloat32Lsb) == SampleFormat::Float32);
    CHECK(sampleFormatForAsioType(asio::kFloat64Lsb) == SampleFormat::Float64);
    CHECK(sampleFormatForAsioType(asio::kInt32Lsb16) == SampleFormat::Int32Lsb16);
    CHECK(sampleFormatForAsioType(asio::kInt32Lsb18) == SampleFormat::Int32Lsb18);
    CHECK(sampleFormatForAsioType(asio::kInt32Lsb20) == SampleFormat::Int32Lsb20);
    CHECK(sampleFormatForAsioType(asio::kInt32Lsb24) == SampleFormat::Int32Lsb24);
    for (const std::int32_t type : {asio::kInt16Msb, asio::kInt24Msb, asio::kInt32Msb, asio::kFloat32Msb,
             asio::kFloat64Msb, asio::kInt32Msb16, asio::kInt32Msb18, asio::kInt32Msb20, asio::kInt32Msb24,
             asio::kDsdInt8Lsb1, asio::kDsdInt8Msb1, asio::kDsdInt8Ner8, 99})
        CHECK_FALSE(sampleFormatForAsioType(type).has_value());

    CHECK(asioSampleTypeName(asio::kInt24Msb) == "Int24MSB");
    CHECK(asioSampleTypeName(asio::kFloat64Lsb) == "Float64LSB");
    CHECK(asioSampleTypeName(99) == "type 99");
    CHECK(asioErrorName(asio::kNoClock) == "ASE_NoClock");
    CHECK(asioErrorName(asio::kNotPresent) == "ASE_NotPresent");
    CHECK(asioErrorName(asio::kHwMalfunction) == "ASE_HWMalfunction");
    CHECK(asioErrorName(asio::kOk) == "ASE_OK");
    CHECK(asioErrorName(-1) == "ASIO error -1");
}

TEST_CASE("ASIO buffer size: the requested one if the driver allows it, else its preferred", "[io][asio]")
{
    SECTION("powers of two from the minimum")
    {
        const AsioBufferSizes sizes{64, 2048, 512, -1};
        CHECK(chooseAsioBufferSize(256, sizes) == 256);
        CHECK(chooseAsioBufferSize(64, sizes) == 64);
        CHECK(chooseAsioBufferSize(2048, sizes) == 2048);
        CHECK(chooseAsioBufferSize(300, sizes) == 512);
        CHECK(chooseAsioBufferSize(32, sizes) == 512);
        CHECK(chooseAsioBufferSize(4096, sizes) == 512);
        CHECK(chooseAsioBufferSize(0, sizes) == 512);
        CHECK(chooseAsioBufferSize(-256, sizes) == 512);
    }
    SECTION("powers of two from a minimum that is not one")
    {
        const AsioBufferSizes sizes{96, 1536, 192, -1};
        CHECK(chooseAsioBufferSize(384, sizes) == 384);
        CHECK(chooseAsioBufferSize(256, sizes) == 192);
    }
    SECTION("a fixed step")
    {
        const AsioBufferSizes sizes{32, 1024, 480, 16};
        CHECK(chooseAsioBufferSize(256, sizes) == 256);
        CHECK(chooseAsioBufferSize(48, sizes) == 48);
        CHECK(chooseAsioBufferSize(250, sizes) == 480);
        CHECK(chooseAsioBufferSize(1040, sizes) == 480);
    }
    SECTION("one size only")
    {
        const AsioBufferSizes sizes{512, 512, 512, 0};
        CHECK(chooseAsioBufferSize(256, sizes) == 512);
        CHECK(chooseAsioBufferSize(512, sizes) == 512);
    }
    SECTION("a range without a step allows only the preferred size")
    {
        const AsioBufferSizes sizes{64, 1024, 128, 0};
        CHECK(chooseAsioBufferSize(256, sizes) == 128);
        CHECK(chooseAsioBufferSize(128, sizes) == 128);
    }
    SECTION("an invalid preferred size falls back to the minimum; nothing usable is 0")
    {
        CHECK(chooseAsioBufferSize(100, AsioBufferSizes{64, 1024, 5000, -1}) == 64);
        CHECK(chooseAsioBufferSize(256, AsioBufferSizes{}) == 0);
    }
    CHECK(isValidAsioBufferSize(256, AsioBufferSizes{64, 2048, 512, -1}));
    CHECK_FALSE(isValidAsioBufferSize(192, AsioBufferSizes{64, 2048, 512, -1}));
}

TEST_CASE("asioMessage answers the host questions and only flags resets", "[io][asio]")
{
    AsioMessageState state;
    state.accepting.store(true);
    for (const std::int32_t selector : {asio::kSelectorSupported, asio::kEngineVersion, asio::kResetRequest,
             asio::kBufferSizeChange, asio::kResyncRequest, asio::kLatenciesChanged, asio::kSupportsTimeInfo})
        CHECK(handleAsioMessage(asio::kSelectorSupported, selector, state) == 1);
    for (const std::int32_t selector :
        {asio::kSupportsTimeCode, asio::kMmcCommand, asio::kSupportsInputMonitor, asio::kOverload, 0, 99})
        CHECK(handleAsioMessage(asio::kSelectorSupported, selector, state) == 0);
    CHECK(handleAsioMessage(asio::kEngineVersion, 0, state) == 2);
    CHECK(handleAsioMessage(asio::kSupportsTimeInfo, 0, state) == 1);
    CHECK(handleAsioMessage(asio::kSupportsTimeCode, 0, state) == 0);
    CHECK(handleAsioMessage(99, 0, state) == 0);
    CHECK(state.resets.load() == 0); // questions flag nothing
    CHECK(state.notices.load() == 0);

    // Latency changes and resyncs are notices, not resets.
    CHECK(handleAsioMessage(asio::kLatenciesChanged, 0, state) == 1);
    CHECK(handleAsioMessage(asio::kResyncRequest, 0, state) == 1);
    CHECK(state.resets.load() == 0);
    CHECK(state.notices.load() == (bit(AsioNotice::Latencies) | bit(AsioNotice::Resync)));

    CHECK(handleAsioMessage(asio::kResetRequest, 0, state) == 1);
    CHECK(state.resets.load() == bit(AsioReset::Request));
    CHECK(handleAsioMessage(asio::kBufferSizeChange, 1024, state) == 1);
    CHECK(state.resets.load() == (bit(AsioReset::Request) | bit(AsioReset::BufferSize)));
    CHECK(state.bufferSize.load() == 1024);
    CHECK(handleAsioMessage(asio::kBufferSizeChange, 0, state) == 1); // no size given: keeps the last one
    CHECK(state.bufferSize.load() == 1024);

    // While not accepting (a stream being set up), the answers stay and nothing is recorded.
    state.clear();
    state.accepting.store(false);
    for (const std::int32_t selector :
        {asio::kResetRequest, asio::kBufferSizeChange, asio::kResyncRequest, asio::kLatenciesChanged})
        CHECK(handleAsioMessage(selector, 512, state) == 1);
    CHECK(handleAsioMessage(asio::kEngineVersion, 0, state) == 2);
    CHECK(state.resets.load() == 0);
    CHECK(state.notices.load() == 0);
    CHECK(state.bufferSize.load() == 0);
}

// ---- the session over a fake driver

namespace {

struct Rendered {
    float left = 0.0f;
    float right = 0.0f;
    std::atomic<int> calls{0};
};

void renderConstant(void* user, float* interleaved, int numChannels, int numFrames) noexcept
{
    auto* r = static_cast<Rendered*>(user);
    for (int i = 0; i < numFrames; ++i) {
        interleaved[i * numChannels] = r->left;
        if (numChannels > 1)
            interleaved[i * numChannels + 1] = r->right;
        for (int ch = 2; ch < numChannels; ++ch)
            interleaved[i * numChannels + ch] = 0.0f;
    }
    r->calls.fetch_add(1);
}

} // namespace

TEST_CASE("An ASIO session opens at the requested rate and buffer, on its own thread", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    Rendered rendered;
    {
        AsioSession session(std::make_unique<FakeAsioDriver>(state));
        CHECK(session.driverNames() == state->names);

        AsioOpenRequest request;
        request.sampleRate = 48000;
        request.bufferFrames = 256;
        request.outputChannels = 2;
        AsioStreamInfo info;
        std::string error;
        REQUIRE(session.open(request, &renderConstant, &rendered, info, error));
        CHECK(state->loadedName == "Fake ASIO"); // empty name = the first driver
        CHECK(state->setRateCalls == 1);
        CHECK(info.sampleRate == 48000);
        CHECK(info.bufferFrames == 256);
        CHECK(state->createdFrames == 256);
        CHECK(info.outputChannels == 2);
        CHECK(info.inputChannels == 0);
        CHECK(info.inputLatency == 300);
        CHECK(info.outputLatency == 400);
        CHECK(info.outputReady);
        CHECK(info.driverName == "Fake ASIO");
        REQUIRE(state->buffers.size() == 2);

        // Both output halves are cleared at open.
        for (const AsioBuffer& buffer : state->buffers)
            for (void* half : buffer.halves)
                for (std::size_t i = 0; i < 256; ++i)
                    CHECK(readInt32(half, i) == 0);

        // A second open in the same session (process) fails.
        AsioStreamInfo other;
        CHECK_FALSE(session.open(request, &renderConstant, &rendered, other, error));
        CHECK(contains(error, "already open"));

        CHECK(session.start(error));
        CHECK(state->started);
        CHECK(session.isRunning());
        CHECK(session.showControlPanel(error));
        CHECK(state->controlPanelCalls == 1);
        session.close();
        CHECK_FALSE(state->loaded);
        CHECK_FALSE(state->buffersCreated);
        CHECK_FALSE(session.isRunning());
    }
    // Every driver call came from the one driver thread, not the test's.
    CHECK(state->comIn);
    CHECK(state->comOut);
    REQUIRE(state->threads.size() == 1);
    CHECK(*state->threads.begin() != std::this_thread::get_id());
}

TEST_CASE("An ASIO session keeps the driver's rate and preferred buffer when asked to", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    Rendered rendered;
    AsioSession session(std::make_unique<FakeAsioDriver>(state));
    AsioOpenRequest request;
    request.sampleRate = 0; // the driver's current rate
    request.bufferFrames = 300; // not a power of two: the preferred 512
    request.outputChannels = 8; // the driver has 4
    request.inputChannels = 6; // the driver has 2
    AsioStreamInfo info;
    std::string error;
    REQUIRE(session.open(request, &renderConstant, &rendered, info, error));
    CHECK(state->setRateCalls == 0);
    CHECK(info.sampleRate == 44100);
    CHECK(info.bufferFrames == 512);
    CHECK(info.outputChannels == 4);
    CHECK(info.inputChannels == 2);
    CHECK(state->buffers.size() == 6);
    CHECK(state->buffers[0].isInput);
    CHECK_FALSE(state->buffers[2].isInput);
}

TEST_CASE("An ASIO session names the driver and the ASIO error when it cannot open", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    Rendered rendered;
    AsioSession session(std::make_unique<FakeAsioDriver>(state));
    AsioOpenRequest request;
    AsioStreamInfo info;
    std::string error;

    SECTION("an unsupported rate")
    {
        request.sampleRate = 96000;
        CHECK_FALSE(session.open(request, &renderConstant, &rendered, info, error));
        CHECK(contains(error, "Fake ASIO"));
        CHECK(contains(error, "96000 Hz"));
        CHECK(contains(error, "ASE_NoClock"));
    }
    SECTION("a big-endian sample format")
    {
        state->outputType = asio::kInt24Msb;
        CHECK_FALSE(session.open(request, &renderConstant, &rendered, info, error));
        CHECK(contains(error, "Fake ASIO"));
        CHECK(contains(error, "Int24MSB"));
        CHECK(contains(error, "output 1"));
    }
    SECTION("a driver that fails to load")
    {
        state->loadResult = asio::kHwMalfunction;
        state->loadMessage = "No device connected";
        CHECK_FALSE(session.open(request, &renderConstant, &rendered, info, error));
        CHECK(contains(error, "Fake ASIO"));
        CHECK(contains(error, "No device connected"));
        CHECK(contains(error, "ASE_HWMalfunction"));
    }
    SECTION("an unknown driver")
    {
        request.driverName = "Missing";
        CHECK_FALSE(session.open(request, &renderConstant, &rendered, info, error));
        CHECK(contains(error, "Missing"));
    }
    SECTION("no outputs")
    {
        state->outputs = 0;
        CHECK_FALSE(session.open(request, &renderConstant, &rendered, info, error));
        CHECK(contains(error, "no outputs"));
    }
    // A failed open leaves nothing loaded.
    CHECK_FALSE(state->loaded);
    CHECK_FALSE(state->buffersCreated);
    CHECK_FALSE(session.isOpen());
}

TEST_CASE("bufferSwitch converts input to float and output into the driver's format", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    state->sizes = AsioBufferSizes{4, 4, 4, 0};
    Rendered rendered;
    rendered.left = 0.5f;
    rendered.right = -2.0f; // clipped to -1
    AsioSession session(std::make_unique<FakeAsioDriver>(state));
    AsioOpenRequest request;
    request.sampleRate = 44100;
    request.inputChannels = 2;
    AsioStreamInfo info;
    std::string error;
    REQUIRE(session.open(request, &renderConstant, &rendered, info, error));
    REQUIRE(info.bufferFrames == 4);
    REQUIRE(state->buffers.size() == 4); // 2 inputs, then 2 outputs

    struct Captured {
        std::vector<float> samples;
        int channels = 0;
        int frames = 0;
        int renderCallsBefore = -1;
        Rendered* rendered = nullptr;
    } captured;
    captured.samples.resize(8);
    captured.rendered = &rendered;
    auto capture = [](void* user, const float* interleaved, int numChannels, int numFrames) noexcept {
        auto* c = static_cast<Captured*>(user);
        c->channels = numChannels;
        c->frames = numFrames;
        c->renderCallsBefore = c->rendered->calls.load();
        std::copy_n(interleaved, numChannels * numFrames, c->samples.begin());
    };
    CHECK_FALSE(session.attachInput(capture, &captured, info.generation + 1, error)); // another stream's
    CHECK(contains(error, "opened again"));
    REQUIRE(session.attachInput(capture, &captured, info.generation, error));
    CHECK(session.isInputAttached(&captured));
    void* other = &rendered;
    CHECK_FALSE(session.attachInput(capture, other, info.generation, error)); // one input at a time
    session.detachInput(other); // not the attached one: no effect
    CHECK(session.isInputAttached(&captured));

    // Input half 1: left 16384, right -32767.
    for (std::size_t i = 0; i < 4; ++i) {
        writeInt16(state->buffers[0].halves[1], i, 16384);
        writeInt16(state->buffers[1].halves[1], i, -32767);
    }

    // Not started: the driver's calls are ignored.
    state->host->bufferSwitch(1);
    CHECK(rendered.calls.load() == 0);

    REQUIRE(session.start(error));
    state->host->bufferSwitch(1);
    CHECK(rendered.calls.load() == 1);
    CHECK(state->outputReadyCalls.load() >= 1);

    // The input came first, as float, interleaved.
    CHECK(captured.renderCallsBefore == 0);
    CHECK(captured.channels == 2);
    CHECK(captured.frames == 4);
    CHECK(captured.samples[0] == static_cast<float>(16384.0 / 32767.0));
    CHECK(captured.samples[1] == -1.0f);

    // The output went into half 1 only, as int32, clipped.
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(readInt32(state->buffers[2].halves[1], i) == 1073741824); // 0.5 * (2^31 - 1), rounded
        CHECK(readInt32(state->buffers[3].halves[1], i) == -2147483647);
        CHECK(readInt32(state->buffers[2].halves[0], i) == 0);
    }

    session.detachInput(&captured);
    CHECK_FALSE(session.isInputAttached(&captured));
    captured.frames = 0;
    state->host->bufferSwitch(0);
    CHECK(captured.frames == 0); // detached: no more input
    CHECK(rendered.calls.load() == 2);

    session.stop();
    state->host->bufferSwitch(1);
    CHECK(rendered.calls.load() == 2); // stopped: no more rendering
}

TEST_CASE("A driver without ASIOOutputReady is not called for it while running", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    state->outputReadySupported = false;
    Rendered rendered;
    AsioSession session(std::make_unique<FakeAsioDriver>(state));
    AsioStreamInfo info;
    std::string error;
    REQUIRE(session.open({}, &renderConstant, &rendered, info, error));
    CHECK_FALSE(info.outputReady);
    const int probes = state->outputReadyCalls.load(); // the probe at open
    REQUIRE(session.start(error));
    state->host->bufferSwitch(0);
    state->host->bufferSwitch(1);
    CHECK(state->outputReadyCalls.load() == probes);
}

TEST_CASE("Driver messages flag a reset; questions and a same-rate change do not", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    Rendered rendered;
    AsioSession session(std::make_unique<FakeAsioDriver>(state));
    AsioStreamInfo info;
    std::string error;
    AsioOpenRequest request;
    request.sampleRate = 44100;
    REQUIRE(session.open(request, &renderConstant, &rendered, info, error));
    CHECK(session.resetReasons() == 0);
    CHECK(state->host->message(asio::kSupportsTimeInfo, 0) == 1);
    state->host->sampleRateChanged(44100.0);
    CHECK(session.resetReasons() == 0);
    state->host->sampleRateChanged(48000.0);
    CHECK(session.resetReasons() == bit(AsioReset::SampleRate));
    CHECK(state->host->message(asio::kResyncRequest, 0) == 1);
    CHECK(session.resetReasons() == bit(AsioReset::SampleRate)); // a resync is only noted
    CHECK(session.notices() == bit(AsioNotice::Resync));
    // A reopen starts clean.
    session.close();
    REQUIRE(session.open(request, &renderConstant, &rendered, info, error));
    CHECK(session.resetReasons() == 0);
}

// ---- AudioDevice and AudioInput with AudioApi::Asio, over the fake

TEST_CASE("AudioDevice opens ASIO, chunks large buffers and reports latency", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    state->sizes = AsioBufferSizes{8192, 8192, 8192, 0}; // larger than kMaxBlockFrames
    state->outputType = asio::kFloat32Lsb;
    const FakeAsioFactory factory(state);

    vloom::AudioDevice device;
    CHECK(vloom::AudioDevice::isApiAvailable(vloom::AudioApi::Asio));
    const auto drivers = device.listOutputDevices(vloom::AudioApi::Asio);
    REQUIRE(drivers.size() == 2);
    CHECK(drivers[0].name == "Fake ASIO");
    CHECK(drivers[0].isDefault);
    CHECK(drivers[1].name == "Other ASIO");

    std::atomic<long long> frames{0};
    std::atomic<int> maxBlock{0};
    vloom::AudioDeviceConfig config;
    config.api = vloom::AudioApi::Asio;
    config.deviceIndex = 1;
    config.sampleRate = 44100;
    config.exclusive = true; // ignored by ASIO
    std::string error;
    REQUIRE(device.open(
        config,
        [&](float* const* out, int channels, int numFrames) {
            for (int ch = 0; ch < channels; ++ch)
                std::fill_n(out[ch], numFrames, 0.25f);
            frames += numFrames;
            maxBlock = std::max(maxBlock.load(), numFrames);
        },
        &error));
    INFO(error);
    CHECK(state->loadedName == "Other ASIO");
    CHECK(device.api() == vloom::AudioApi::Asio);
    CHECK(device.deviceName() == "Other ASIO");
    CHECK(device.sampleRate() == 44100);
    CHECK(device.channels() == 2);
    CHECK(device.bufferFrames() == 8192);
    CHECK(device.inputLatencyFrames() == 300);
    CHECK(device.outputLatencyFrames() == 400);
    CHECK_FALSE(device.isExclusive());
    CHECK_FALSE(device.resetRequested());

    REQUIRE(device.start(&error));
    CHECK(device.isRunning());
    state->host->bufferSwitch(0);
    CHECK(frames.load() == 8192);
    CHECK(maxBlock.load() == vloom::AudioDevice::kMaxBlockFrames);
    float sample = 0.0f;
    std::memcpy(&sample, static_cast<const std::byte*>(state->buffers[0].halves[0]) + 4 * 8191, 4);
    CHECK(sample == 0.25f);

    state->host->message(asio::kResetRequest, 0);
    CHECK(device.resetRequested());
    CHECK(device.showControlPanel(&error));
    CHECK(state->controlPanelCalls == 1);

    // Only one ASIO driver per process.
    vloom::AudioDevice second;
    CHECK_FALSE(second.open(config, [](float* const*, int, int) { }, &error));
    CHECK(contains(error, "already open"));
    CHECK(device.isRunning());

    device.close();
    CHECK_FALSE(device.isOpen());
    CHECK(device.api() == vloom::AudioApi::Default);
    CHECK(device.bufferFrames() == 0);
    CHECK_FALSE(state->loaded);
    CHECK_FALSE(device.showControlPanel(&error));
}

TEST_CASE("An ASIO AudioInput taps the output's driver", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    state->sizes = AsioBufferSizes{16, 16, 16, 0};
    const FakeAsioFactory factory(state);

    vloom::AudioInputConfig inputConfig;
    inputConfig.api = vloom::AudioApi::Asio;
    inputConfig.sampleRate = 0;
    std::atomic<int> inputBlocks{0};
    std::atomic<int> inputChannels{0};
    std::atomic<int> outputBlocks{0};
    std::atomic<int> outputBlocksAtInput{-1};
    auto onInput = [&](const float* const*, int channels, int) {
        inputChannels = channels;
        outputBlocksAtInput = outputBlocks.load();
        ++inputBlocks;
    };
    std::string error;

    vloom::AudioInput input;
    CHECK(input.listInputDevices(vloom::AudioApi::Asio).size() == 2);
    CHECK_FALSE(input.open(inputConfig, onInput, &error)); // no output yet
    CHECK(contains(error, "output first"));

    vloom::AudioDevice device;
    vloom::AudioDeviceConfig config;
    config.api = vloom::AudioApi::Asio;
    config.sampleRate = 44100;
    auto onOutput = [&](float* const*, int, int) { ++outputBlocks; };
    REQUIRE(device.open(config, onOutput, &error));
    CHECK_FALSE(input.open(inputConfig, onInput, &error)); // the output has no inputs
    CHECK(contains(error, "inputChannels"));
    device.close();

    config.inputChannels = 2;
    REQUIRE(device.open(config, onOutput, &error));
    vloom::AudioInputConfig wrongRate = inputConfig;
    wrongRate.sampleRate = 48000;
    CHECK_FALSE(input.open(wrongRate, onInput, &error));
    CHECK(contains(error, "44100"));
    vloom::AudioInputConfig otherDriver = inputConfig;
    otherDriver.deviceIndex = 1;
    CHECK_FALSE(input.open(otherDriver, onInput, &error));
    CHECK(contains(error, "only one ASIO driver"));

    inputConfig.channels = 1;
    REQUIRE(input.open(inputConfig, onInput, &error));
    CHECK(input.api() == vloom::AudioApi::Asio);
    CHECK(input.sampleRate() == 44100);
    CHECK(input.channels() == 1);
    CHECK(input.deviceName() == "Fake ASIO");
    REQUIRE(input.start(&error));
    CHECK_FALSE(input.isRunning()); // the output is not started yet
    REQUIRE(device.start(&error));
    CHECK(input.isRunning());

    state->host->bufferSwitch(0);
    state->host->bufferSwitch(1);
    CHECK(inputBlocks.load() == 2);
    CHECK(outputBlocks.load() == 2);
    CHECK(inputChannels.load() == 1);
    CHECK(outputBlocksAtInput.load() == 1); // the input of a period comes before its output

    input.stop();
    state->host->bufferSwitch(0);
    CHECK(inputBlocks.load() == 2);
    REQUIRE(input.start(&error));
    state->host->bufferSwitch(1);
    CHECK(inputBlocks.load() == 3);

    device.close(); // the input stops with its output
    CHECK_FALSE(input.isRunning());
    input.close();
    CHECK_FALSE(input.isOpen());
}

TEST_CASE(
    "What a driver says while opening or starting is no reset; latency changes are asked again", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    FakeState* raw = state.get();
    state->duringCreate = [](AsioDriverHost& host) {
        host.message(asio::kResetRequest, 0);
        host.message(asio::kLatenciesChanged, 0);
        host.message(asio::kBufferSizeChange, 128);
        host.sampleRateChanged(96000.0);
    };
    state->duringStart = [raw] {
        raw->host->message(asio::kResetRequest, 0);
        raw->host->message(asio::kBufferSizeChange, 64);
    };
    const FakeAsioFactory factory(state);

    vloom::AudioDevice device;
    vloom::AudioDeviceConfig config;
    config.api = vloom::AudioApi::Asio;
    config.sampleRate = 44100;
    std::string error;
    REQUIRE(device.open(config, [](float* const*, int, int) { }, &error));
    CHECK_FALSE(device.resetRequested());
    CHECK(device.bufferFrames() == 256);
    CHECK(device.reopenBufferFrames() == 256);
    REQUIRE(device.start(&error));
    CHECK_FALSE(device.resetRequested());
    CHECK(device.reopenBufferFrames() == 256);

    CHECK(device.inputLatencyFrames() == 300);
    CHECK(device.outputLatencyFrames() == 400);
    state->inputLatency = 500;
    state->outputLatency = 700;
    CHECK(device.outputLatencyFrames() == 400); // nothing reported: the values from open
    CHECK(state->host->message(asio::kLatenciesChanged, 0) == 1);
    CHECK_FALSE(device.resetRequested());
    CHECK(device.outputLatencyFrames() == 700);
    CHECK(device.inputLatencyFrames() == 500);

    CHECK(state->host->message(asio::kResyncRequest, 0) == 1);
    CHECK_FALSE(device.resetRequested());
    CHECK(device.isRunning());
}

TEST_CASE("After a buffer-size change the device reopens at the driver's new size", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    const FakeAsioFactory factory(state);
    vloom::AudioDevice device;
    vloom::AudioDeviceConfig config;
    config.api = vloom::AudioApi::Asio;
    config.sampleRate = 44100;
    std::string error;
    auto callback = [](float* const*, int, int) { };
    REQUIRE(device.open(config, callback, &error));
    REQUIRE(device.start(&error));
    CHECK(device.bufferFrames() == 256);

    SECTION("the size the message names")
    {
        CHECK(state->host->message(asio::kBufferSizeChange, 1024) == 1);
        CHECK(device.resetRequested());
        CHECK(device.reopenBufferFrames() == 1024);
    }
    SECTION("no size in the message: the driver's preferred size now")
    {
        state->sizes.preferred = 128;
        CHECK(state->host->message(asio::kBufferSizeChange, 0) == 1);
        CHECK(device.resetRequested());
        CHECK(device.reopenBufferFrames() == 128);
    }
    config.bufferFrames = device.reopenBufferFrames();
    device.close();
    CHECK(device.reopenBufferFrames() == 0);
    REQUIRE(device.open(config, callback, &error));
    CHECK(device.bufferFrames() == config.bufferFrames);
    CHECK_FALSE(device.resetRequested());
}

TEST_CASE("An ASIO input must be opened again after its output was", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    const FakeAsioFactory factory(state);
    vloom::AudioDevice device;
    vloom::AudioDeviceConfig config;
    config.api = vloom::AudioApi::Asio;
    config.sampleRate = 44100;
    config.inputChannels = 2;
    std::string error;
    auto onOutput = [](float* const*, int, int) { };
    auto onInput = [](const float* const*, int, int) { };
    REQUIRE(device.open(config, onOutput, &error));

    vloom::AudioInput input;
    vloom::AudioInputConfig inputConfig;
    inputConfig.api = vloom::AudioApi::Asio;
    inputConfig.sampleRate = 0;
    REQUIRE(input.open(inputConfig, onInput, &error));
    CHECK(input.sampleRate() == 44100);

    // The output comes back at another rate: the input's view is stale.
    device.close();
    config.sampleRate = 48000;
    REQUIRE(device.open(config, onOutput, &error));
    CHECK_FALSE(input.start(&error));
    CHECK(contains(error, "opened again"));

    REQUIRE(input.open(inputConfig, onInput, &error));
    CHECK(input.sampleRate() == 48000);
    CHECK(input.start(&error));
}

TEST_CASE("An exception while opening unloads the driver and says why", "[io][asio]")
{
    auto state = std::make_shared<FakeState>();
    state->throwInCreate = true;
    Rendered rendered;
    AsioSession session(std::make_unique<FakeAsioDriver>(state));
    AsioStreamInfo info;
    std::string error;
    CHECK_FALSE(session.open({}, &renderConstant, &rendered, info, error));
    CHECK(contains(error, "fake driver failure"));
    CHECK_FALSE(state->loaded);
    CHECK_FALSE(session.isOpen());

    state->throwInCreate = false;
    CHECK(session.open({}, &renderConstant, &rendered, info, error));
    CHECK(state->loaded);
}

TEST_CASE("A new ASIO session waits until the previous one is fully gone", "[io][asio]")
{
    // The SDK keeps globals: a new driver thread must not start while the
    // old one still tears down.
    auto first = std::make_shared<FakeState>();
    first->stopDelayMs = 100;
    auto second = std::make_shared<FakeState>();
    second->previous = first;
    int made = 0;
    setAsioDriverFactoryForTesting(
        [&made, first, second] { return std::make_unique<FakeAsioDriver>(made++ == 0 ? first : second); });

    std::shared_ptr<AsioSession> session = acquireAsioSession();
    REQUIRE(session != nullptr);
    CHECK(acquireAsioSession() == session); // one per process

    std::thread releaser([held = std::move(session)]() mutable { held.reset(); });
    while (!first->stoppingStarted.load())
        std::this_thread::yield();
    std::shared_ptr<AsioSession> next = acquireAsioSession();
    REQUIRE(next != nullptr);
    releaser.join();
    next.reset();
    setAsioDriverFactoryForTesting(nullptr);

    CHECK(made == 2);
    CHECK(first->stopped.load());
    CHECK(second->previousWasStopped);
}

TEST_CASE("Without an ASIO driver factory, ASIO is what the build says", "[io][asio]")
{
#if defined(_WIN32)
    // Windows: available only in builds with VLOOM_ENGINE_ASIO. Nothing here
    // lists or loads the real drivers.
    const std::string reason = asioUnavailableReason();
    CHECK(vloom::AudioDevice::isApiAvailable(vloom::AudioApi::Asio) == reason.empty());
#else
    CHECK_FALSE(vloom::AudioDevice::isApiAvailable(vloom::AudioApi::Asio));
    CHECK(std::string(asioUnavailableReason()) == "ASIO is not available on this platform");
    vloom::AudioDevice device;
    CHECK(device.listOutputDevices(vloom::AudioApi::Asio).empty());
    vloom::AudioDeviceConfig config;
    config.api = vloom::AudioApi::Asio;
    std::string error;
    CHECK_FALSE(device.open(config, [](float* const*, int, int) { }, &error));
    CHECK(error == "ASIO is not available on this platform");
    vloom::AudioInput input;
    CHECK(input.listInputDevices(vloom::AudioApi::Asio).empty());
    vloom::AudioInputConfig inputConfig;
    inputConfig.api = vloom::AudioApi::Asio;
    CHECK_FALSE(input.open(inputConfig, [](const float* const*, int, int) { }, &error));
    CHECK(error == "ASIO is not available on this platform");
#endif
    CHECK(vloom::AudioDevice::isApiAvailable(vloom::AudioApi::Default));
}
