// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/io/MidiInput.hpp"

#include "io/MidiBackend.hpp"

#include <atomic>
#include <mutex>

namespace vloom {

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
namespace io {
std::vector<std::string> midiInputNames() { return {}; }
std::unique_ptr<MidiBackend> makeMidiBackend(MidiDeliver) { return nullptr; }
} // namespace io
#endif

/// What the backend's thread shares with the MidiInput: the caller's
/// callback, run only while started, under a lock that stop() and close()
/// take too, so neither returns while it runs. Shared, not owned: a
/// callback the OS runs late (CoreMIDI may, while a port is disposed of)
/// still finds it, stopped, after the MidiInput is gone.
struct MidiShared {
    std::mutex mutex;
    MidiInput::Callback callback;
    std::atomic<bool> running{false}; ///< written under `mutex`

    void deliver(const MidiMessage& message)
    {
        if (message.status < 0x80 || message.status >= 0xF0)
            return;
        const std::lock_guard lock(mutex);
        if (running && callback)
            callback(message);
    }
};

struct MidiInput::Impl {
    std::unique_ptr<io::MidiBackend> backend;
    std::shared_ptr<MidiShared> state = std::make_shared<MidiShared>();
};

MidiInput::MidiInput()
    : impl_(std::make_unique<Impl>())
{
}

MidiInput::~MidiInput() { close(); }

std::vector<MidiInputInfo> MidiInput::listDevices()
{
    std::vector<MidiInputInfo> devices;
    for (std::string& name : io::midiInputNames())
        devices.push_back(MidiInputInfo{std::move(name)});
    return devices;
}

bool MidiInput::open(int index, Callback callback, std::string* error)
{
    close();
    Impl* impl = impl_.get();
    auto backend
        = io::makeMidiBackend([state = impl->state](const MidiMessage& message) { state->deliver(message); });
    if (backend == nullptr) {
        if (error != nullptr)
            *error = "MIDI input is not supported on this system";
        return false;
    }
    if (!backend->open(index, error))
        return false;
    {
        const std::lock_guard lock(impl->state->mutex);
        impl->state->callback = std::move(callback);
    }
    impl->backend = std::move(backend);
    return true;
}

bool MidiInput::start(std::string* error)
{
    if (impl_->backend == nullptr) {
        if (error != nullptr)
            *error = "no MIDI input is open";
        return false;
    }
    {
        const std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->running)
            return true;
        impl_->state->running = true;
    }
    if (impl_->backend->start(error))
        return true;
    const std::lock_guard lock(impl_->state->mutex);
    impl_->state->running = false;
    return false;
}

void MidiInput::stop()
{
    {
        const std::lock_guard lock(impl_->state->mutex);
        if (impl_->state->running && impl_->state->callback) {
            // Keys still down would otherwise sound on: let go of them all.
            for (int channel = 0; channel < 16; ++channel) {
                try {
                    impl_->state->callback(MidiMessage{static_cast<std::uint8_t>(0xB0 | channel), 123, 0});
                } catch (...) {
                }
            }
        }
        impl_->state->running = false;
    }
    if (impl_->backend != nullptr)
        impl_->backend->stop();
}

void MidiInput::close()
{
    stop();
    if (impl_->backend != nullptr) {
        impl_->backend->close();
        impl_->backend.reset();
    }
    const std::lock_guard lock(impl_->state->mutex);
    impl_->state->callback = nullptr;
}

bool MidiInput::isOpen() const noexcept { return impl_->backend != nullptr; }

bool MidiInput::isRunning() const noexcept { return impl_->state->running.load(std::memory_order_relaxed); }

std::string MidiInput::deviceName() const { return impl_->backend != nullptr ? impl_->backend->name() : ""; }

} // namespace vloom
