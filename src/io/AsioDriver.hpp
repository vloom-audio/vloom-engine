// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

// Internal: an ASIO driver as the engine uses it (ADR 0021), without the
// Steinberg SDK. AsioSdkDriver.cpp implements it with the SDK on Windows;
// the tests implement it with a fake. Everything here, and the session on
// top of it (AsioSession.hpp), is plain C++ that builds on every platform.

#include "io/SampleFormat.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vloom::io {

/// Values of the ASIO 2.3 interface (the SDK's common/asio.h), so that code
/// which only needs the numbers does not need the SDK.
namespace asio {

// ASIOError
inline constexpr std::int32_t kOk = 0;
inline constexpr std::int32_t kSuccess = 0x3f4847a0; ///< ASIOFuture's "done"
inline constexpr std::int32_t kNotPresent = -1000;
inline constexpr std::int32_t kHwMalfunction = -999;
inline constexpr std::int32_t kInvalidParameter = -998;
inline constexpr std::int32_t kInvalidMode = -997;
inline constexpr std::int32_t kSpNotAdvancing = -996;
inline constexpr std::int32_t kNoClock = -995;
inline constexpr std::int32_t kNoMemory = -994;

// ASIOSampleType
inline constexpr std::int32_t kInt16Msb = 0;
inline constexpr std::int32_t kInt24Msb = 1;
inline constexpr std::int32_t kInt32Msb = 2;
inline constexpr std::int32_t kFloat32Msb = 3;
inline constexpr std::int32_t kFloat64Msb = 4;
inline constexpr std::int32_t kInt32Msb16 = 8;
inline constexpr std::int32_t kInt32Msb18 = 9;
inline constexpr std::int32_t kInt32Msb20 = 10;
inline constexpr std::int32_t kInt32Msb24 = 11;
inline constexpr std::int32_t kInt16Lsb = 16;
inline constexpr std::int32_t kInt24Lsb = 17;
inline constexpr std::int32_t kInt32Lsb = 18;
inline constexpr std::int32_t kFloat32Lsb = 19;
inline constexpr std::int32_t kFloat64Lsb = 20;
inline constexpr std::int32_t kInt32Lsb16 = 24;
inline constexpr std::int32_t kInt32Lsb18 = 25;
inline constexpr std::int32_t kInt32Lsb20 = 26;
inline constexpr std::int32_t kInt32Lsb24 = 27;
inline constexpr std::int32_t kDsdInt8Lsb1 = 32;
inline constexpr std::int32_t kDsdInt8Msb1 = 33;
inline constexpr std::int32_t kDsdInt8Ner8 = 40;

// asioMessage selectors
inline constexpr std::int32_t kSelectorSupported = 1;
inline constexpr std::int32_t kEngineVersion = 2;
inline constexpr std::int32_t kResetRequest = 3;
inline constexpr std::int32_t kBufferSizeChange = 4;
inline constexpr std::int32_t kResyncRequest = 5;
inline constexpr std::int32_t kLatenciesChanged = 6;
inline constexpr std::int32_t kSupportsTimeInfo = 7;
inline constexpr std::int32_t kSupportsTimeCode = 8;
inline constexpr std::int32_t kMmcCommand = 9;
inline constexpr std::int32_t kSupportsInputMonitor = 10;
inline constexpr std::int32_t kOverload = 16;

/// The ASIO version the host implements, answered to kEngineVersion.
inline constexpr std::int32_t kHostAsioVersion = 2;

} // namespace asio

/// Why the driver wants the stream reopened: bits of
/// AsioMessageState::resets.
enum class AsioReset : std::uint32_t {
    Request = 1U << 0U, ///< kAsioResetRequest: settings changed in the driver's panel
    BufferSize = 1U << 1U, ///< kAsioBufferSizeChange
    SampleRate = 1U << 2U, ///< sampleRateDidChange: the clock changed under the stream
};

/// What the driver reported that needs no reopen: bits of
/// AsioMessageState::notices. Many drivers send these while their buffers
/// are created or the stream starts.
enum class AsioNotice : std::uint32_t {
    Latencies = 1U << 0U, ///< kAsioLatenciesChanged: query ASIOGetLatencies again
    Resync = 1U << 1U, ///< kAsioResyncRequest: the driver lost sync and recovered
};

[[nodiscard]] constexpr std::uint32_t bit(AsioReset reason) noexcept
{
    return static_cast<std::uint32_t>(reason);
}

[[nodiscard]] constexpr std::uint32_t bit(AsioNotice notice) noexcept
{
    return static_cast<std::uint32_t>(notice);
}

/// What asioMessage() records, read by the control thread. Lock-free.
struct AsioMessageState {
    /// Off while a stream is being opened or closed: what the driver says
    /// then belongs to the setup, not to the open stream.
    std::atomic<bool> accepting{false};
    std::atomic<std::uint32_t> resets{0}; ///< AsioReset bits
    std::atomic<std::uint32_t> notices{0}; ///< AsioNotice bits
    std::atomic<int> bufferSize{0}; ///< the size kAsioBufferSizeChange named; 0 = none

    void clear() noexcept
    {
        resets.store(0);
        notices.store(0);
        bufferSize.store(0);
    }
};

/// The ASE_ name of an ASIO error ("ASE_NoClock"), or "ASIO error <n>".
[[nodiscard]] std::string asioErrorName(std::int32_t error);

/// The ASIOST name of a sample type ("Int24MSB"), or "type <n>".
[[nodiscard]] std::string asioSampleTypeName(std::int32_t type);

/// The format a little-endian ASIO sample type maps to. Empty for the
/// big-endian (MSB) and DSD types, which the engine does not convert.
[[nodiscard]] std::optional<SampleFormat> sampleFormatForAsioType(std::int32_t type) noexcept;

/// ASIOGetBufferSize's answer.
struct AsioBufferSizes {
    int minSize = 0;
    int maxSize = 0;
    int preferred = 0;
    /// Step between valid sizes from minSize; -1 = powers of two; 0 = only
    /// the one size (minSize == maxSize).
    int granularity = 0;
};

/// True when `frames` is a size the driver allows.
[[nodiscard]] bool isValidAsioBufferSize(int frames, const AsioBufferSizes& sizes) noexcept;

/// `requested` when the driver allows it, else the driver's preferred size
/// (or, from a driver whose preferred size is itself invalid, its minimum).
/// 0 when the driver reports no usable size.
[[nodiscard]] int chooseAsioBufferSize(int requested, const AsioBufferSizes& sizes) noexcept;

/// The host's side of asioMessage(). Answers kAsioSelectorSupported,
/// kAsioEngineVersion (2) and kAsioSupportsTimeInfo (yes). A reset request
/// or buffer-size change records a reset (and the new size), a latency
/// change or resync records a notice, and each returns 1: a control thread
/// acts on them, never this callback. Nothing is recorded while
/// `state.accepting` is off. Everything else returns 0. Real-time safe.
std::int32_t handleAsioMessage(std::int32_t selector, std::int32_t value, AsioMessageState& state) noexcept;

/// One channel's double buffer, as ASIOCreateBuffers fills it.
struct AsioBuffer {
    bool isInput = false;
    int channel = 0; ///< the driver's channel index
    std::array<void*, 2> halves{}; ///< filled by createBuffers()
};

/// What the driver calls back, on threads of its own.
class AsioDriverHost {
public:
    AsioDriverHost() = default;
    AsioDriverHost(const AsioDriverHost&) = delete;
    AsioDriverHost& operator=(const AsioDriverHost&) = delete;

    /// bufferSwitch / bufferSwitchTimeInfo: read input half `index` and fill
    /// output half `index`. Real-time.
    virtual void bufferSwitch(int index) noexcept = 0;
    /// asioMessage.
    virtual std::int32_t message(std::int32_t selector, std::int32_t value) noexcept = 0;
    /// sampleRateDidChange (0 = the clock was lost).
    virtual void sampleRateChanged(double rate) noexcept = 0;

protected:
    ~AsioDriverHost() = default;
};

/// One ASIO driver, as the SDK's ASIO* functions see it. Only one can be
/// loaded per process. Every call but outputReady() is made on the one
/// thread that called threadStarted() (ASIO drivers are COM objects that
/// expect a single-threaded apartment); results are ASIO error codes.
class AsioDriver {
public:
    AsioDriver() = default;
    virtual ~AsioDriver() = default;
    AsioDriver(const AsioDriver&) = delete;
    AsioDriver& operator=(const AsioDriver&) = delete;

    /// On the driver thread, first and last: COM in and out.
    virtual void threadStarted() { }
    virtual void threadStopping() { }
    /// On the driver thread, between tasks: waits up to `timeout` for
    /// wakeIdle() (a COM apartment also dispatches its window messages
    /// meanwhile). A wakeIdle() before the wait is not lost.
    virtual void waitIdle(std::chrono::milliseconds timeout);
    /// From any thread: work is queued for the driver thread.
    virtual void wakeIdle();

    /// The installed drivers' names, in the SDK's form (what load() takes).
    [[nodiscard]] virtual std::vector<std::string> driverNames() = 0;
    /// Loads and initialises (ASIOInit) the named driver. On failure
    /// `message` may hold the driver's own text.
    virtual std::int32_t load(const std::string& name, std::string& message) = 0;
    /// ASIOExit. Safe when nothing is loaded.
    virtual void unload() noexcept = 0;

    virtual std::int32_t getChannels(int& inputs, int& outputs) = 0;
    virtual std::int32_t getBufferSize(AsioBufferSizes& sizes) = 0;
    virtual std::int32_t canSampleRate(double rate) = 0;
    virtual std::int32_t getSampleRate(double& rate) = 0;
    virtual std::int32_t setSampleRate(double rate) = 0;
    virtual std::int32_t getChannelType(bool isInput, int channel, std::int32_t& type) = 0;
    /// Creates every channel's double buffer and connects `host`'s callbacks.
    virtual std::int32_t createBuffers(std::vector<AsioBuffer>& buffers, int frames, AsioDriverHost& host)
        = 0;
    virtual std::int32_t disposeBuffers() = 0;
    virtual std::int32_t getLatencies(int& input, int& output) = 0;
    virtual std::int32_t start() = 0;
    virtual std::int32_t stop() = 0;
    virtual std::int32_t controlPanel() = 0;
    /// ASIOOutputReady. kOk when the driver supports it. Real-time: called
    /// from bufferSwitch().
    virtual std::int32_t outputReady() noexcept = 0;

private:
    std::mutex idleMutex_;
    std::condition_variable idleChanged_;
    bool woken_ = false;
};

} // namespace vloom::io
