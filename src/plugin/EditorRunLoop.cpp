// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/EditorRunLoop.hpp"

#include <algorithm>

#if !defined(_WIN32)
#include <poll.h>
#endif

namespace vloom::plugin_editor {

std::uint32_t RunLoop::addTimer(std::chrono::milliseconds period, std::function<void(std::uint32_t)> fire)
{
    if (timers_.size() >= kMaxEntries || !fire)
        return 0;
    period = std::max(period, std::chrono::milliseconds{0});
    const std::uint32_t id = nextId_++;
    if (nextId_ == 0)
        nextId_ = 1;
    timers_.push_back(Timer{id, period, std::chrono::steady_clock::now() + period, std::move(fire)});
    return id;
}

bool RunLoop::removeTimer(std::uint32_t id)
{
    const auto it = std::find_if(timers_.begin(), timers_.end(), [id](const Timer& t) { return t.id == id; });
    if (it == timers_.end())
        return false;
    timers_.erase(it);
    return true;
}

bool RunLoop::addFd(int fd, std::uint32_t conditions, std::function<void(std::uint32_t)> ready)
{
#if defined(_WIN32)
    (void)fd;
    (void)conditions;
    (void)ready;
    return false;
#else
    if (fd < 0 || fds_.size() >= kMaxEntries || !ready
        || std::any_of(fds_.begin(), fds_.end(), [fd](const Fd& f) { return f.fd == fd; }))
        return false;
    fds_.push_back(Fd{fd, conditions, std::move(ready)});
    return true;
#endif
}

bool RunLoop::modifyFd(int fd, std::uint32_t conditions)
{
    const auto it = std::find_if(fds_.begin(), fds_.end(), [fd](const Fd& f) { return f.fd == fd; });
    if (it == fds_.end())
        return false;
    it->conditions = conditions;
    return true;
}

bool RunLoop::removeFd(int fd)
{
    const auto it = std::find_if(fds_.begin(), fds_.end(), [fd](const Fd& f) { return f.fd == fd; });
    if (it == fds_.end())
        return false;
    fds_.erase(it);
    return true;
}

void RunLoop::run()
{
    // By id, so a callback that adds or removes entries is safe.
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> due;
    for (const Timer& t : timers_)
        if (t.due <= now)
            due.push_back(t.id);
    for (const std::uint32_t id : due) {
        const auto it
            = std::find_if(timers_.begin(), timers_.end(), [id](const Timer& t) { return t.id == id; });
        if (it == timers_.end())
            continue; // removed by an earlier callback
        it->due = now + it->period;
        const std::function<void(std::uint32_t)> fire = it->fire; // the entry may go away while it runs
        fire(id);
    }

#if !defined(_WIN32)
    if (fds_.empty())
        return;
    std::vector<pollfd> polled;
    for (const Fd& f : fds_) {
        pollfd p{};
        p.fd = f.fd;
        p.events = static_cast<short>(
            ((f.conditions & kRead) != 0 ? POLLIN : 0) | ((f.conditions & kWrite) != 0 ? POLLOUT : 0));
        polled.push_back(p);
    }
    if (::poll(polled.data(), static_cast<nfds_t>(polled.size()), 0) <= 0)
        return;
    for (const pollfd& p : polled) {
        const std::uint32_t happened = ((p.revents & POLLIN) != 0 ? kRead : 0)
            | ((p.revents & POLLOUT) != 0 ? kWrite : 0)
            | ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ? kError : 0);
        if (happened == 0)
            continue;
        const auto it = std::find_if(fds_.begin(), fds_.end(), [&p](const Fd& f) { return f.fd == p.fd; });
        if (it == fds_.end())
            continue;
        const std::function<void(std::uint32_t)> ready = it->ready;
        ready(happened);
    }
#endif
}

} // namespace vloom::plugin_editor
