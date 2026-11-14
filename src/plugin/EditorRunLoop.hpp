// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace vloom::plugin_editor {

/// The timers and file descriptors a plugin registers for its editor (CLAP
/// timer-support and posix-fd-support, VST 3 IRunLoop), run from
/// AudioProcessor::idle() on the control thread (ADR 0009). Not real-time:
/// it may allocate.
class RunLoop {
public:
    /// At most this many timers and descriptors each; more are refused.
    static constexpr std::size_t kMaxEntries = 64;

    /// File descriptor conditions, as CLAP numbers them.
    static constexpr std::uint32_t kRead = 1;
    static constexpr std::uint32_t kWrite = 2;
    static constexpr std::uint32_t kError = 4;

    /// Calls `fire` with the timer's id every `period` (at most as often as
    /// run() is called).
    /// Returns an id above 0, or 0 if refused.
    std::uint32_t addTimer(std::chrono::milliseconds period, std::function<void(std::uint32_t)> fire);
    bool removeTimer(std::uint32_t id);

    /// Calls `ready` with the conditions that hold when `fd` is ready for
    /// any of `conditions`. POSIX only: elsewhere it is refused (false).
    bool addFd(int fd, std::uint32_t conditions, std::function<void(std::uint32_t)> ready);
    bool modifyFd(int fd, std::uint32_t conditions);
    bool removeFd(int fd);

    /// Fires due timers and ready descriptors. Callbacks may add or remove
    /// entries.
    void run();

    [[nodiscard]] bool empty() const noexcept { return timers_.empty() && fds_.empty(); }

private:
    struct Timer {
        std::uint32_t id = 0;
        std::chrono::milliseconds period{};
        std::chrono::steady_clock::time_point due;
        std::function<void(std::uint32_t)> fire;
    };
    struct Fd {
        int fd = -1;
        std::uint32_t conditions = 0;
        std::function<void(std::uint32_t)> ready;
    };

    std::vector<Timer> timers_;
    std::vector<Fd> fds_;
    std::uint32_t nextId_ = 1;
};

} // namespace vloom::plugin_editor
