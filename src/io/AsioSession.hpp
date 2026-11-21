// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// Internal: the one ASIO driver of the process, shared by the output and
// the input that taps it (ADR 0021). Plain C++ on top of AsioDriver, so it
// builds and is tested on every platform; only the driver behind it is
// Windows and SDK code.

#include "io/AsioDriver.hpp"
#include "io/AudioBackend.hpp"
#include "io/InputBackend.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vloom::io {

struct AsioOpenRequest {
    std::string driverName; ///< as AsioDriver::driverNames() lists it; empty = the first driver
    int sampleRate = 48000; ///< 0 = the driver's current rate
    int bufferFrames = 256; ///< used when the driver allows it, else its preferred size
    int outputChannels = 2; ///< from the driver's first output
    int inputChannels = 0; ///< from the driver's first input, for an input that taps the stream
};

/// What open() got.
struct AsioStreamInfo {
    std::string driverName;
    int sampleRate = 0;
    int bufferFrames = 0;
    int outputChannels = 0;
    int inputChannels = 0;
    int inputLatency = 0; ///< frames, ASIOGetLatencies
    int outputLatency = 0; ///< frames, ASIOGetLatencies
    bool outputReady = false; ///< the driver takes ASIOOutputReady()
    /// Counts the session's successful opens: an input opened against one
    /// stream cannot attach to the next (its rate or inputs may differ).
    std::uint64_t generation = 0;
};

/// Owns the loaded driver and a thread of its own that makes every call to
/// it (load, buffers, start, stop, unload, control panel): COM apartments
/// tie a driver to the thread that created it. Control calls from any
/// thread; they run one at a time and block until the driver thread has
/// done them. A modal control panel therefore blocks every other control
/// call, stop() and close() included, until the user closes it.
///
/// The driver's bufferSwitch converts the input halves into float and hands
/// them to the attached input, then renders the output and converts it into
/// the output halves. No allocation and no locks there.
class AsioSession final : private AsioDriverHost {
public:
    explicit AsioSession(std::unique_ptr<AsioDriver> driver);
    ~AsioSession();
    AsioSession(const AsioSession&) = delete;
    AsioSession& operator=(const AsioSession&) = delete;

    [[nodiscard]] std::vector<std::string> driverNames();

    /// Loads the driver and creates its buffers (does not start). Fails if
    /// a stream is already open in this session. What the driver reports
    /// while opening is not taken as a reset.
    bool open(const AsioOpenRequest& request, RenderFn render, void* user, AsioStreamInfo& info,
        std::string& error);
    /// Starts the stream; resets the driver asked for while starting are
    /// dropped.
    bool start(std::string& error);
    /// Returns once bufferSwitch no longer renders.
    void stop() noexcept;
    /// Disposes the buffers and unloads the driver; detaches the input.
    void close() noexcept;

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }
    [[nodiscard]] AsioStreamInfo info() const;

    /// ASIOControlPanel on the open driver. See the class comment: a modal
    /// panel blocks other control calls until it is closed.
    bool showControlPanel(std::string& error);

    /// AsioReset bits the driver reported since open(); not 0 = reopen.
    [[nodiscard]] std::uint32_t resetReasons() const noexcept { return messages_.resets.load(); }
    /// AsioNotice bits since open() (latency changes, resyncs).
    [[nodiscard]] std::uint32_t notices() const noexcept { return messages_.notices.load(); }

    /// The open stream's latencies; after kAsioLatenciesChanged, asks the
    /// driver again first. False when nothing is open.
    bool latencies(int& input, int& output) noexcept;

    /// The buffer size to reopen with: after kAsioBufferSizeChange, the
    /// size the message named, else the driver's preferred size now; else
    /// the open stream's size. 0 when nothing is open.
    [[nodiscard]] int reopenBufferFrames() noexcept;

    /// Starts handing the open stream's input to `capture` (interleaved,
    /// inputChannels wide, bufferFrames long) from bufferSwitch. Fails when
    /// nothing is open, it has no inputs, or the stream is not the one of
    /// `generation` (AsioStreamInfo::generation). One input at a time.
    bool attachInput(CaptureFn capture, void* user, std::uint64_t generation, std::string& error);
    /// Returns once `capture` is no longer being called. Only detaches the
    /// input whose `user` it is.
    void detachInput(void* user) noexcept;
    [[nodiscard]] bool isInputAttached(void* user) const noexcept;

private:
    void bufferSwitch(int index) noexcept override;
    std::int32_t message(std::int32_t selector, std::int32_t value) noexcept override;
    void sampleRateChanged(double rate) noexcept override;

    void driverThread();
    /// Runs `task` on the driver thread and waits for it.
    void call(const std::function<void()>& task);

    bool openOnDriverThread(const AsioOpenRequest& request, RenderFn render, void* user, std::string& error);
    bool openSteps(const AsioOpenRequest& request, RenderFn render, void* user, std::string& error,
        bool& loaded, bool& created);
    void closeOnDriverThread() noexcept;
    void waitForCallbacks() const noexcept;

    std::unique_ptr<AsioDriver> driver_;

    // The driver thread and its one pending task.
    std::thread thread_;
    std::mutex callMutex_; ///< one control call at a time
    std::mutex taskMutex_;
    std::condition_variable taskChanged_;
    const std::function<void()>* task_ = nullptr;
    bool taskDone_ = false;
    bool quit_ = false;

    // Set on the driver thread while nothing calls back; read by bufferSwitch.
    mutable std::mutex stateMutex_; ///< control threads only, never bufferSwitch
    bool loaded_ = false; ///< guarded by stateMutex_
    bool buffersCreated_ = false; ///< driver thread only
    std::uint64_t generation_ = 0; ///< guarded by stateMutex_
    AsioStreamInfo info_;
    RenderFn render_ = nullptr;
    void* renderUser_ = nullptr;
    std::vector<AsioBuffer> buffers_; ///< inputs first, then outputs
    std::vector<SampleFormat> formats_; ///< one per buffer
    std::vector<float> outputScratch_; ///< bufferFrames * outputChannels, interleaved
    std::vector<float> inputScratch_; ///< bufferFrames * inputChannels, interleaved
    std::vector<float> planar_; ///< bufferFrames, one channel at a time

    struct InputTap {
        CaptureFn capture = nullptr;
        void* user = nullptr;
    };
    InputTap tap_; ///< written under stateMutex_ only while not attached
    std::atomic<const InputTap*> attached_{nullptr};

    std::atomic<bool> running_{false};
    std::atomic<int> streamRate_{0}; ///< for sampleRateChanged(), which may come at any time
    mutable std::atomic<int> inCallback_{0};
    AsioMessageState messages_;
};

/// Makes the real driver: the Steinberg SDK's on Windows builds with
/// VLOOM_ENGINE_ASIO, else null.
[[nodiscard]] std::unique_ptr<AsioDriver> makeAsioDriver();

/// Why ASIO cannot be used in this build, or empty when it can.
[[nodiscard]] const char* asioUnavailableReason() noexcept;

/// The session of the process, made on first use and kept while anyone
/// holds it. A new one is made only once the previous one is fully
/// destroyed (its driver thread joined), since the SDK keeps globals. Null
/// when ASIO is unavailable.
[[nodiscard]] std::shared_ptr<AsioSession> acquireAsioSession();

/// Tests only: makes acquireAsioSession() use `factory` (null = the real
/// driver again). Call while no session is held.
void setAsioDriverFactoryForTesting(std::function<std::unique_ptr<AsioDriver>()> factory);

/// vloom::AudioDevice's ASIO output: opens the process's session.
[[nodiscard]] std::unique_ptr<AudioBackend> createAsioBackend();

/// vloom::AudioInput's ASIO input: taps the session the output opened.
[[nodiscard]] std::unique_ptr<InputBackend> createAsioInputBackend();

} // namespace vloom::io
