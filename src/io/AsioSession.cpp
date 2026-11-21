// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "io/AsioSession.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>

namespace vloom::io {

namespace {

constexpr int kMaxChannels = 8; ///< as AudioDevice and AudioInput
/// The longest the idle driver thread sleeps without being woken; queued
/// work and (on Windows) window messages wake it at once.
constexpr auto kIdleTimeout = std::chrono::milliseconds(100);

std::string driverError(const std::string& driver, const std::string& what, std::int32_t error)
{
    return "ASIO driver '" + driver + "' " + what + " (" + asioErrorName(error) + ")";
}

std::string channelLabel(bool isInput, int channel)
{
    return std::string(isInput ? "input " : "output ") + std::to_string(channel + 1);
}

} // namespace

AsioSession::AsioSession(std::unique_ptr<AsioDriver> driver)
    : driver_(std::move(driver))
{
    thread_ = std::thread([this] { driverThread(); });
}

AsioSession::~AsioSession()
{
    close();
    {
        const std::lock_guard lock(taskMutex_);
        quit_ = true;
    }
    driver_->wakeIdle();
    thread_.join();
}

void AsioSession::driverThread()
{
    driver_->threadStarted();
    for (;;) {
        const std::function<void()>* task = nullptr;
        {
            const std::lock_guard lock(taskMutex_);
            if (quit_)
                break;
            if (task_ != nullptr && !taskDone_)
                task = task_;
        }
        if (task == nullptr) {
            driver_->waitIdle(kIdleTimeout);
            continue;
        }
        try {
            (*task)();
        } catch (...) {
            // A control call must not end the driver thread; it reports
            // its own failure through its captures.
        }
        {
            const std::lock_guard lock(taskMutex_);
            taskDone_ = true;
        }
        taskChanged_.notify_all();
    }
    driver_->threadStopping();
}

void AsioSession::call(const std::function<void()>& task)
{
    const std::lock_guard callLock(callMutex_);
    {
        const std::lock_guard lock(taskMutex_);
        task_ = &task;
        taskDone_ = false;
    }
    driver_->wakeIdle();
    std::unique_lock lock(taskMutex_);
    taskChanged_.wait(lock, [this] { return taskDone_; });
    task_ = nullptr;
}

std::vector<std::string> AsioSession::driverNames()
{
    std::vector<std::string> names;
    call([&] { names = driver_->driverNames(); });
    return names;
}

bool AsioSession::open(
    const AsioOpenRequest& request, RenderFn render, void* user, AsioStreamInfo& info, std::string& error)
{
    if (render == nullptr) {
        error = "No render callback given";
        return false;
    }
    bool ok = false;
    call([&] { ok = openOnDriverThread(request, render, user, error); });
    if (ok)
        info = this->info();
    return ok;
}

bool AsioSession::openOnDriverThread(
    const AsioOpenRequest& request, RenderFn render, void* user, std::string& error)
{
    bool loaded = false;
    bool created = false;
    std::string failure;
    try {
        return openSteps(request, render, user, error, loaded, created);
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "unknown error";
    }
    // Something threw half way: undo what was done.
    try {
        if (created)
            driver_->disposeBuffers();
        if (loaded)
            driver_->unload();
    } catch (...) {
    }
    try {
        error = "Could not open the ASIO driver: " + failure;
    } catch (...) {
    }
    return false;
}

bool AsioSession::openSteps(const AsioOpenRequest& request, RenderFn render, void* user, std::string& error,
    bool& loaded, bool& created)
{
    {
        const std::lock_guard lock(stateMutex_);
        if (loaded_) {
            error = "ASIO driver '" + info_.driverName + "' is already open in this process";
            return false;
        }
    }
    // What the driver says while it is set up is not about the open stream.
    messages_.accepting.store(false);
    const std::vector<std::string> names = driver_->driverNames();
    if (names.empty()) {
        error = "No ASIO driver is installed";
        return false;
    }
    const std::string name = request.driverName.empty() ? names.front() : request.driverName;
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        error = "ASIO driver '" + name + "' is not installed";
        return false;
    }

    std::string message;
    const std::int32_t loadResult = driver_->load(name, message);
    if (loadResult != asio::kOk) {
        error = "Could not load ASIO driver '" + name + "'" + (message.empty() ? "" : ": " + message) + " ("
            + asioErrorName(loadResult) + ")";
        return false;
    }
    loaded = true;

    // From here on a failure unloads the driver again.
    auto fail = [&](std::string text) {
        if (created)
            driver_->disposeBuffers();
        created = false;
        driver_->unload();
        loaded = false;
        error = std::move(text);
        return false;
    };

    int inputs = 0;
    int outputs = 0;
    if (const std::int32_t result = driver_->getChannels(inputs, outputs); result != asio::kOk)
        return fail(driverError(name, "could not report its channels", result));
    if (outputs <= 0)
        return fail("ASIO driver '" + name + "' has no outputs");

    double current = 0.0;
    const std::int32_t rateResult = driver_->getSampleRate(current);
    double rate = current;
    if (request.sampleRate > 0) {
        rate = static_cast<double>(request.sampleRate);
        if (rateResult != asio::kOk || current != rate) {
            const std::string hz = std::to_string(request.sampleRate) + " Hz";
            if (const std::int32_t result = driver_->canSampleRate(rate); result != asio::kOk)
                return fail(driverError(name, "does not support " + hz, result));
            if (const std::int32_t result = driver_->setSampleRate(rate); result != asio::kOk)
                return fail(driverError(name, "could not switch to " + hz, result));
        }
    } else if (rateResult != asio::kOk || !(current > 0.0)) {
        return fail(driverError(
            name, "reports no sample rate", rateResult == asio::kOk ? asio::kNoClock : rateResult));
    }

    AsioBufferSizes sizes;
    if (const std::int32_t result = driver_->getBufferSize(sizes); result != asio::kOk)
        return fail(driverError(name, "could not report its buffer sizes", result));
    const int frames = chooseAsioBufferSize(request.bufferFrames, sizes);
    if (frames <= 0)
        return fail("ASIO driver '" + name + "' reports no usable buffer size");

    const int outputChannels = std::clamp(request.outputChannels, 1, std::min(outputs, kMaxChannels));
    const int inputChannels
        = std::clamp(request.inputChannels, 0, std::min(std::max(inputs, 0), kMaxChannels));

    std::vector<AsioBuffer> buffers;
    std::vector<SampleFormat> formats;
    buffers.reserve(static_cast<std::size_t>(inputChannels + outputChannels));
    formats.reserve(buffers.capacity());
    auto addChannel = [&](bool isInput, int channel) -> bool {
        std::int32_t type = 0;
        if (const std::int32_t result = driver_->getChannelType(isInput, channel, type);
            result != asio::kOk) {
            error = driverError(name, "could not describe " + channelLabel(isInput, channel), result);
            return false;
        }
        const std::optional<SampleFormat> format = sampleFormatForAsioType(type);
        if (!format) {
            error = "ASIO driver '" + name + "' uses the sample format " + asioSampleTypeName(type) + " on "
                + channelLabel(isInput, channel) + ", which the engine does not support";
            return false;
        }
        AsioBuffer buffer;
        buffer.isInput = isInput;
        buffer.channel = channel;
        buffers.push_back(buffer);
        formats.push_back(*format);
        return true;
    };
    for (int ch = 0; ch < inputChannels; ++ch)
        if (!addChannel(true, ch))
            return fail(error);
    for (int ch = 0; ch < outputChannels; ++ch)
        if (!addChannel(false, ch))
            return fail(error);

    // Everything bufferSwitch needs exists before the driver can call it.
    const auto frameCount = static_cast<std::size_t>(frames);
    std::vector<float> outputScratch(frameCount * static_cast<std::size_t>(outputChannels), 0.0f);
    std::vector<float> inputScratch(frameCount * static_cast<std::size_t>(inputChannels), 0.0f);
    std::vector<float> planar(frameCount, 0.0f);

    streamRate_.store(static_cast<int>(std::lround(rate)));
    const std::int32_t createResult = driver_->createBuffers(buffers, frames, *this);
    created = createResult == asio::kOk;
    if (!created)
        return fail(driverError(
            name, "could not create buffers of " + std::to_string(frames) + " frames", createResult));
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        for (void* half : buffers[i].halves)
            if (half == nullptr)
                return fail("ASIO driver '" + name + "' gave no buffer for "
                    + channelLabel(buffers[i].isInput, buffers[i].channel));
        // Both output halves start silent: the driver may play one before the first bufferSwitch.
        if (!buffers[i].isInput)
            for (void* half : buffers[i].halves)
                std::memset(half, 0, static_cast<std::size_t>(bytesPerSample(formats[i])) * frameCount);
    }

    int inputLatency = 0;
    int outputLatency = 0;
    if (driver_->getLatencies(inputLatency, outputLatency) != asio::kOk) {
        inputLatency = 0;
        outputLatency = 0;
    }

    AsioStreamInfo stream;
    stream.driverName = name;
    stream.sampleRate = static_cast<int>(std::lround(rate));
    stream.bufferFrames = frames;
    stream.outputChannels = outputChannels;
    stream.inputChannels = inputChannels;
    stream.inputLatency = std::max(inputLatency, 0);
    stream.outputLatency = std::max(outputLatency, 0);
    stream.outputReady = driver_->outputReady() == asio::kOk;

    // Nothing calls back into these until start(); nothing below throws.
    const std::lock_guard lock(stateMutex_);
    stream.generation = ++generation_;
    loaded_ = true;
    buffersCreated_ = true;
    info_ = std::move(stream);
    render_ = render;
    renderUser_ = user;
    buffers_ = std::move(buffers);
    formats_ = std::move(formats);
    outputScratch_ = std::move(outputScratch);
    inputScratch_ = std::move(inputScratch);
    planar_ = std::move(planar);
    messages_.clear();
    messages_.accepting.store(true);
    return true;
}

bool AsioSession::start(std::string& error)
{
    bool ok = false;
    call([&] {
        if (!buffersCreated_) {
            error = "No ASIO driver is open";
            return;
        }
        if (running_.load()) {
            ok = true;
            return;
        }
        running_.store(true);
        if (const std::int32_t result = driver_->start(); result != asio::kOk) {
            running_.store(false);
            waitForCallbacks();
            error = driverError(info_.driverName, "could not start", result);
            return;
        }
        // Drivers may ask for a reset while they start: that is not one.
        messages_.resets.store(0);
        messages_.bufferSize.store(0);
        ok = true;
    });
    return ok;
}

void AsioSession::stop() noexcept
{
    try {
        call([&] {
            if (!running_.load())
                return;
            driver_->stop();
            running_.store(false);
            waitForCallbacks();
        });
    } catch (...) {
        // Only the mutexes can throw; there is nothing left to stop then.
    }
}

void AsioSession::close() noexcept
{
    try {
        call([&] { closeOnDriverThread(); });
    } catch (...) {
    }
}

void AsioSession::closeOnDriverThread() noexcept
{
    bool wasLoaded = false;
    {
        // First, so that no input attaches to a stream on its way out.
        const std::lock_guard lock(stateMutex_);
        wasLoaded = loaded_;
        loaded_ = false;
    }
    messages_.accepting.store(false);
    try {
        if (running_.load()) {
            driver_->stop();
            running_.store(false);
        }
        attached_.store(nullptr);
        waitForCallbacks();
        if (buffersCreated_)
            driver_->disposeBuffers();
        buffersCreated_ = false;
        if (wasLoaded)
            driver_->unload();
    } catch (...) {
        running_.store(false);
        buffersCreated_ = false;
    }
    const std::lock_guard lock(stateMutex_);
    info_ = {};
    render_ = nullptr;
    renderUser_ = nullptr;
    buffers_.clear();
    formats_.clear();
    streamRate_.store(0);
    messages_.clear();
}

void AsioSession::waitForCallbacks() const noexcept
{
    while (inCallback_.load() != 0)
        std::this_thread::yield();
}

bool AsioSession::isOpen() const
{
    const std::lock_guard lock(stateMutex_);
    return loaded_;
}

AsioStreamInfo AsioSession::info() const
{
    const std::lock_guard lock(stateMutex_);
    return info_;
}

bool AsioSession::showControlPanel(std::string& error)
{
    bool ok = false;
    call([&] {
        if (!buffersCreated_) {
            error = "No ASIO driver is open";
            return;
        }
        if (const std::int32_t result = driver_->controlPanel(); result != asio::kOk) {
            error = driverError(info_.driverName, "could not show its control panel", result);
            return;
        }
        ok = true;
    });
    return ok;
}

bool AsioSession::latencies(int& input, int& output) noexcept
{
    try {
        if ((messages_.notices.load() & bit(AsioNotice::Latencies)) != 0) {
            call([&] {
                if (!buffersCreated_)
                    return;
                // Cleared first: a change reported during the query is not lost.
                messages_.notices.fetch_and(~bit(AsioNotice::Latencies));
                int in = 0;
                int out = 0;
                if (driver_->getLatencies(in, out) != asio::kOk)
                    return;
                const std::lock_guard lock(stateMutex_);
                info_.inputLatency = std::max(in, 0);
                info_.outputLatency = std::max(out, 0);
            });
        }
        const std::lock_guard lock(stateMutex_);
        if (!loaded_)
            return false;
        input = info_.inputLatency;
        output = info_.outputLatency;
        return true;
    } catch (...) {
        return false;
    }
}

int AsioSession::reopenBufferFrames() noexcept
{
    try {
        int frames = 0;
        if ((messages_.resets.load() & bit(AsioReset::BufferSize)) != 0) {
            frames = messages_.bufferSize.load();
            if (frames <= 0)
                call([&] {
                    AsioBufferSizes sizes;
                    if (buffersCreated_ && driver_->getBufferSize(sizes) == asio::kOk)
                        frames = chooseAsioBufferSize(sizes.preferred, sizes);
                });
        }
        if (frames > 0)
            return frames;
        const std::lock_guard lock(stateMutex_);
        return loaded_ ? info_.bufferFrames : 0;
    } catch (...) {
        return 0;
    }
}

bool AsioSession::attachInput(CaptureFn capture, void* user, std::uint64_t generation, std::string& error)
{
    const std::lock_guard lock(stateMutex_);
    if (!loaded_) {
        error = "No ASIO driver is open: an ASIO input taps the driver the ASIO output opened";
        return false;
    }
    if (generation != info_.generation) {
        error = "The ASIO output was opened again since this input was opened: open the input again";
        return false;
    }
    if (info_.inputChannels <= 0) {
        error = "ASIO driver '" + info_.driverName
            + "' was opened without inputs (set AudioDeviceConfig::inputChannels)";
        return false;
    }
    if (capture == nullptr) {
        error = "No capture callback given";
        return false;
    }
    if (const InputTap* current = attached_.load(); current != nullptr) {
        if (current->user == user)
            return true;
        error = "Another input already taps ASIO driver '" + info_.driverName + "'";
        return false;
    }
    // Not attached: bufferSwitch cannot be reading tap_ now.
    tap_.capture = capture;
    tap_.user = user;
    attached_.store(&tap_);
    return true;
}

void AsioSession::detachInput(void* user) noexcept
{
    {
        const std::lock_guard lock(stateMutex_);
        if (tap_.user != user)
            return;
        const InputTap* expected = &tap_;
        if (!attached_.compare_exchange_strong(expected, nullptr))
            return;
    }
    waitForCallbacks();
}

bool AsioSession::isInputAttached(void* user) const noexcept
{
    const InputTap* current = attached_.load();
    return current != nullptr && current->user == user;
}

// ---- the driver's callbacks

void AsioSession::bufferSwitch(int index) noexcept
{
    // stop() and detachInput() clear their flag first, then wait for this
    // count to drop to 0: a callback that saw the old value is waited for,
    // and one that starts later sees the new one.
    inCallback_.fetch_add(1);
    if (running_.load()) {
        const auto half = static_cast<std::size_t>(index != 0 ? 1 : 0);
        const auto frames = static_cast<std::size_t>(info_.bufferFrames);
        const auto inputs = static_cast<std::size_t>(info_.inputChannels);
        const auto outputs = static_cast<std::size_t>(info_.outputChannels);

        if (const InputTap* tap = attached_.load(); tap != nullptr && inputs > 0) {
            for (std::size_t ch = 0; ch < inputs; ++ch) {
                convertToFloat(formats_[ch], static_cast<const std::byte*>(buffers_[ch].halves[half]),
                    planar_.data(), frames);
                for (std::size_t i = 0; i < frames; ++i)
                    inputScratch_[i * inputs + ch] = planar_[i];
            }
            tap->capture(tap->user, inputScratch_.data(), static_cast<int>(inputs), static_cast<int>(frames));
        }

        render_(renderUser_, outputScratch_.data(), static_cast<int>(outputs), static_cast<int>(frames));
        for (std::size_t ch = 0; ch < outputs; ++ch) {
            for (std::size_t i = 0; i < frames; ++i)
                planar_[i] = outputScratch_[i * outputs + ch];
            convertFromFloat(formats_[inputs + ch], planar_.data(),
                static_cast<std::byte*>(buffers_[inputs + ch].halves[half]), frames);
        }
        if (info_.outputReady)
            driver_->outputReady();
    }
    inCallback_.fetch_sub(1);
}

std::int32_t AsioSession::message(std::int32_t selector, std::int32_t value) noexcept
{
    return handleAsioMessage(selector, value, messages_);
}

void AsioSession::sampleRateChanged(double rate) noexcept
{
    if (!messages_.accepting.load())
        return;
    const int current = streamRate_.load();
    if (current != 0 && (!(rate > 0.0) || std::lround(rate) != current))
        messages_.resets.fetch_or(bit(AsioReset::SampleRate));
}

// ---- the process's session

namespace {

struct Registry {
    std::mutex mutex;
    std::condition_variable gone; ///< a session finished destroying
    std::weak_ptr<AsioSession> session;
    bool alive = false; ///< a session exists, perhaps still being destroyed
    std::function<std::unique_ptr<AsioDriver>()> testFactory;
};

Registry& registry()
{
    static Registry instance;
    return instance;
}

} // namespace

#if !defined(VLOOM_ENGINE_HAS_ASIO)
std::unique_ptr<AsioDriver> makeAsioDriver() { return nullptr; }
#endif

const char* asioUnavailableReason() noexcept
{
    {
        Registry& reg = registry();
        const std::lock_guard lock(reg.mutex);
        if (reg.testFactory)
            return "";
    }
#if defined(VLOOM_ENGINE_HAS_ASIO)
    return "";
#elif defined(_WIN32)
    return "ASIO support is not built in (VLOOM_ENGINE_ASIO is OFF)";
#else
    return "ASIO is not available on this platform";
#endif
}

std::shared_ptr<AsioSession> acquireAsioSession()
{
    Registry& reg = registry();
    std::unique_lock lock(reg.mutex);
    if (std::shared_ptr<AsioSession> session = reg.session.lock())
        return session;
    // The weak pointer expires as the last holder lets go, before the old
    // session's destructor has joined its driver thread: the SDK's globals
    // are not ours until it has.
    reg.gone.wait(lock, [&reg] { return !reg.alive; });
    std::unique_ptr<AsioDriver> driver = reg.testFactory ? reg.testFactory() : makeAsioDriver();
    if (driver == nullptr)
        return nullptr;
    std::shared_ptr<AsioSession> session(new AsioSession(std::move(driver)), [](AsioSession* old) {
        delete old;
        Registry& r = registry();
        {
            const std::lock_guard doneLock(r.mutex);
            r.alive = false;
        }
        r.gone.notify_all();
    });
    reg.alive = true;
    reg.session = session;
    return session;
}

void setAsioDriverFactoryForTesting(std::function<std::unique_ptr<AsioDriver>()> factory)
{
    Registry& reg = registry();
    const std::lock_guard lock(reg.mutex);
    reg.testFactory = std::move(factory);
}

// ---- the backends behind AudioDevice and AudioInput

namespace {

std::vector<BackendDeviceInfo> listDrivers(const std::shared_ptr<AsioSession>& held)
{
    std::vector<BackendDeviceInfo> result;
    const std::shared_ptr<AsioSession> session = held ? held : acquireAsioSession();
    if (session == nullptr)
        return result;
    const std::vector<std::string> names = session->driverNames();
    result.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        result.push_back({names[i], names[i], i == 0});
    return result;
}

class AsioOutputBackend final : public AudioBackend {
public:
    ~AsioOutputBackend() override { close(); }
    AsioOutputBackend() = default;
    AsioOutputBackend(const AsioOutputBackend&) = delete;
    AsioOutputBackend& operator=(const AsioOutputBackend&) = delete;

    std::vector<BackendDeviceInfo> listOutputDevices() override { return listDrivers(session_); }

    bool open(const BackendStreamConfig& config, RenderFn render, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        session_ = acquireAsioSession();
        if (session_ == nullptr) {
            error = asioUnavailableReason();
            return false;
        }
        AsioOpenRequest request;
        request.driverName = config.deviceId;
        request.sampleRate = config.sampleRate;
        request.bufferFrames = config.bufferFrames;
        request.outputChannels = config.channels;
        request.inputChannels = config.inputChannels;
        AsioStreamInfo stream;
        if (!session_->open(request, render, user, stream, error)) {
            session_.reset();
            return false;
        }
        info.sampleRate = stream.sampleRate;
        info.channels = stream.outputChannels;
        info.deviceName = stream.driverName;
        info.exclusive = false;
        info.bufferFrames = stream.bufferFrames;
        info.inputLatency = stream.inputLatency;
        info.outputLatency = stream.outputLatency;
        return true;
    }

    bool start(std::string& error) override
    {
        if (session_ == nullptr) {
            error = "No ASIO driver is open";
            return false;
        }
        return session_->start(error);
    }

    void stop() noexcept override
    {
        if (session_ != nullptr)
            session_->stop();
    }

    void close() noexcept override
    {
        if (session_ == nullptr)
            return;
        session_->close();
        session_.reset();
    }

    [[nodiscard]] bool isRunning() const noexcept override
    {
        return session_ != nullptr && session_->isRunning();
    }

    bool showControlPanel(std::string& error) override
    {
        if (session_ == nullptr) {
            error = "No ASIO driver is open";
            return false;
        }
        return session_->showControlPanel(error);
    }

    [[nodiscard]] bool resetRequested() const noexcept override
    {
        return session_ != nullptr && session_->resetReasons() != 0;
    }

    bool refreshLatency(int& input, int& output) noexcept override
    {
        return session_ != nullptr && session_->latencies(input, output);
    }

    [[nodiscard]] int reopenBufferFrames() noexcept override
    {
        return session_ != nullptr ? session_->reopenBufferFrames() : 0;
    }

private:
    std::shared_ptr<AsioSession> session_; ///< held while open: the stream is ours
};

class AsioInputBackend final : public InputBackend {
public:
    ~AsioInputBackend() override { close(); }
    AsioInputBackend() = default;
    AsioInputBackend(const AsioInputBackend&) = delete;
    AsioInputBackend& operator=(const AsioInputBackend&) = delete;

    std::vector<BackendDeviceInfo> listInputDevices() override { return listDrivers(session_); }

    bool open(const BackendStreamConfig& config, CaptureFn capture, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        std::shared_ptr<AsioSession> session = acquireAsioSession();
        if (session == nullptr) {
            error = asioUnavailableReason();
            return false;
        }
        const AsioStreamInfo stream = session->info();
        if (stream.generation == 0 || !session->isOpen()) {
            error = "Open the ASIO output first: an ASIO input taps the driver the output opened";
            return false;
        }
        if (!config.deviceId.empty() && config.deviceId != stream.driverName) {
            error = "ASIO driver '" + config.deviceId + "' is not the one open for output ('"
                + stream.driverName + "'): only one ASIO driver can be loaded at a time";
            return false;
        }
        if (stream.inputChannels <= 0) {
            error = "ASIO driver '" + stream.driverName
                + "' was opened without inputs (set AudioDeviceConfig::inputChannels)";
            return false;
        }
        if (config.sampleRate > 0 && config.sampleRate != stream.sampleRate) {
            error = "An ASIO input runs at its output's rate, " + std::to_string(stream.sampleRate)
                + " Hz, not " + std::to_string(config.sampleRate) + " Hz";
            return false;
        }
        session_ = std::move(session);
        capture_ = capture;
        user_ = user;
        generation_ = stream.generation;
        info.sampleRate = stream.sampleRate;
        info.channels = std::clamp(config.channels, 1, stream.inputChannels);
        info.deviceName = stream.driverName;
        info.bufferFrames = stream.bufferFrames;
        info.inputLatency = stream.inputLatency;
        info.outputLatency = stream.outputLatency;
        return true;
    }

    bool start(std::string& error) override
    {
        if (session_ == nullptr) {
            error = "No ASIO input is open";
            return false;
        }
        return session_->attachInput(capture_, user_, generation_, error);
    }

    void stop() noexcept override
    {
        if (session_ != nullptr)
            session_->detachInput(user_);
    }

    void close() noexcept override
    {
        stop();
        session_.reset();
        capture_ = nullptr;
        user_ = nullptr;
        generation_ = 0;
    }

    [[nodiscard]] bool isRunning() const noexcept override
    {
        return session_ != nullptr && session_->isRunning() && session_->isInputAttached(user_);
    }

private:
    std::shared_ptr<AsioSession> session_;
    CaptureFn capture_ = nullptr;
    void* user_ = nullptr;
    std::uint64_t generation_ = 0; ///< the output stream this input was opened against
};

} // namespace

std::unique_ptr<AudioBackend> createAsioBackend() { return std::make_unique<AsioOutputBackend>(); }

std::unique_ptr<InputBackend> createAsioInputBackend() { return std::make_unique<AsioInputBackend>(); }

} // namespace vloom::io
