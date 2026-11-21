// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "io/AsioSession.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vloom::io {

namespace {

constexpr int kMaxChannels = 8; ///< as AudioDevice and AudioInput
/// How often the idle driver thread dispatches window messages: some
/// drivers post their notifications to the apartment that loaded them.
constexpr auto kPumpInterval = std::chrono::milliseconds(5);

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
    taskChanged_.notify_all();
    thread_.join();
}

void AsioSession::driverThread()
{
    driver_->threadStarted();
    std::unique_lock lock(taskMutex_);
    while (!quit_) {
        if (task_ != nullptr && !taskDone_) {
            const std::function<void()>* task = task_;
            lock.unlock();
            try {
                (*task)();
            } catch (...) {
                // A control call must not end the driver thread; it reports
                // its own failure through its captures.
            }
            lock.lock();
            taskDone_ = true;
            taskChanged_.notify_all();
            continue;
        }
        taskChanged_.wait_for(lock, kPumpInterval);
        if (!quit_ && (task_ == nullptr || taskDone_)) {
            lock.unlock();
            driver_->pumpMessages();
            lock.lock();
        }
    }
    lock.unlock();
    driver_->threadStopping();
}

void AsioSession::call(const std::function<void()>& task)
{
    const std::lock_guard callLock(callMutex_);
    std::unique_lock lock(taskMutex_);
    task_ = &task;
    taskDone_ = false;
    taskChanged_.notify_all();
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
    if (loaded_) {
        error = "ASIO driver '" + info_.driverName + "' is already open in this process";
        return false;
    }
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
    const std::int32_t loaded = driver_->load(name, message);
    if (loaded != asio::kOk) {
        error = "Could not load ASIO driver '" + name + "'" + (message.empty() ? "" : ": " + message) + " ("
            + asioErrorName(loaded) + ")";
        return false;
    }

    // From here on a failure unloads the driver again.
    bool created = false;
    auto fail = [&](std::string text) {
        if (created)
            driver_->disposeBuffers();
        driver_->unload();
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

    resets_.store(0);
    streamRate_.store(static_cast<int>(std::lround(rate)));
    if (const std::int32_t result = driver_->createBuffers(buffers, frames, *this); result != asio::kOk)
        return fail(
            driverError(name, "could not create buffers of " + std::to_string(frames) + " frames", result));
    created = true;
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        for (void* half : buffers[i].halves)
            if (half == nullptr)
                return fail("ASIO driver '" + name + "' gave no buffer for "
                    + channelLabel(buffers[i].isInput, buffers[i].channel));
        // Both output halves start silent: the driver may play one before the first bufferSwitch.
        if (!buffers[i].isInput)
            for (void* half : buffers[i].halves)
                std::memset(half, 0,
                    static_cast<std::size_t>(bytesPerSample(formats[i])) * static_cast<std::size_t>(frames));
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

    // Nothing calls back into these until start().
    const std::lock_guard lock(stateMutex_);
    loaded_ = true;
    buffersCreated_ = true;
    info_ = stream;
    render_ = render;
    renderUser_ = user;
    buffers_ = std::move(buffers);
    formats_ = std::move(formats);
    const auto frameCount = static_cast<std::size_t>(frames);
    outputScratch_.assign(frameCount * static_cast<std::size_t>(outputChannels), 0.0f);
    inputScratch_.assign(frameCount * static_cast<std::size_t>(inputChannels), 0.0f);
    planar_.assign(frameCount, 0.0f);
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
    if (running_.load()) {
        driver_->stop();
        running_.store(false);
    }
    attached_.store(nullptr);
    waitForCallbacks();
    if (buffersCreated_)
        driver_->disposeBuffers();
    if (loaded_)
        driver_->unload();
    const std::lock_guard lock(stateMutex_);
    loaded_ = false;
    buffersCreated_ = false;
    info_ = {};
    render_ = nullptr;
    renderUser_ = nullptr;
    buffers_.clear();
    formats_.clear();
    streamRate_.store(0);
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
        if (!loaded_) {
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

bool AsioSession::attachInput(CaptureFn capture, void* user, std::string& error)
{
    const std::lock_guard lock(stateMutex_);
    if (!loaded_) {
        error = "No ASIO driver is open: an ASIO input taps the driver the ASIO output opened";
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
    const InputTap* current = attached_.load();
    if (current == nullptr || current->user != user)
        return;
    attached_.store(nullptr);
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
    return handleAsioMessage(selector, value, resets_);
}

void AsioSession::sampleRateChanged(double rate) noexcept
{
    const int current = streamRate_.load();
    if (current != 0 && (!(rate > 0.0) || std::lround(rate) != current))
        resets_.fetch_or(bit(AsioReset::SampleRate));
}

// ---- the process's session

namespace {

struct Registry {
    std::mutex mutex;
    std::weak_ptr<AsioSession> session;
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
    const std::lock_guard lock(reg.mutex);
    if (std::shared_ptr<AsioSession> session = reg.session.lock())
        return session;
    std::unique_ptr<AsioDriver> driver = reg.testFactory ? reg.testFactory() : makeAsioDriver();
    if (driver == nullptr)
        return nullptr;
    auto session = std::make_shared<AsioSession>(std::move(driver));
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
        if (!session->isOpen()) {
            error = "Open the ASIO output first: an ASIO input taps the driver the output opened";
            return false;
        }
        const AsioStreamInfo stream = session->info();
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
        return session_->attachInput(capture_, user_, error);
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
    }

    [[nodiscard]] bool isRunning() const noexcept override
    {
        return session_ != nullptr && session_->isRunning() && session_->isInputAttached(user_);
    }

private:
    std::shared_ptr<AsioSession> session_;
    CaptureFn capture_ = nullptr;
    void* user_ = nullptr;
};

} // namespace

std::unique_ptr<AudioBackend> createAsioBackend() { return std::make_unique<AsioOutputBackend>(); }

std::unique_ptr<InputBackend> createAsioInputBackend() { return std::make_unique<AsioInputBackend>(); }

} // namespace vloom::io
