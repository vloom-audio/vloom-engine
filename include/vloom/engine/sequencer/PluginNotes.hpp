// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace vloom {

/// The notes a channel sends to its instrument plugin (ADR 0006): which keys
/// are held and until when, and the note events of the block being rendered.
/// The same voice interface as Sampler and Synth (held until release()
/// reaches their end), but instead of rendering it collects NoteEvents for
/// PluginInstance::process(). Audio thread only; nothing allocates.
class PluginNotes {
public:
    static constexpr std::int64_t kNoRelease = INT64_MAX;
    /// Events one block holds; further note-ons in the block are dropped.
    static constexpr std::size_t kMaxEvents = 256;

    /// Starts `key` at `frame` of the block, held until release() reaches
    /// `releaseAt` (ticks). A key already held is ended first, so the
    /// plugin sees off then on. Keys out of 0 .. 127 are ignored.
    void noteOn(int frame, int key, float velocity, std::int64_t releaseAt) noexcept
    {
        if (key < 0 || key >= kKeys)
            return;
        // Room for this on and for an off of every key held afterwards, so a
        // held key can always be ended in this block.
        const auto k = static_cast<std::size_t>(key);
        const bool retrigger = releaseAt_[k] != kNotHeld;
        if (count_ + static_cast<std::size_t>(held_) + (retrigger ? 1 : 2) > kMaxEvents)
            return;
        if (retrigger)
            noteOff(frame, k);
        events_[count_++] = NoteEvent{frame, NoteEvent::Type::On, static_cast<std::int16_t>(key), velocity};
        releaseAt_[k] = releaseAt;
        ++held_;
    }

    /// The smallest releaseAt among the held keys; kNoRelease if none.
    [[nodiscard]] std::int64_t nextRelease() const noexcept
    {
        std::int64_t next = kNoRelease;
        if (held_ > 0)
            for (const std::int64_t at : releaseAt_)
                if (at != kNotHeld && at < next)
                    next = at;
        return next;
    }
    /// Ends, at `frame`, the held keys whose releaseAt is at or before `upTo`.
    void release(std::int64_t upTo, int frame) noexcept
    {
        if (held_ > 0)
            for (std::size_t k = 0; k < releaseAt_.size(); ++k)
                if (releaseAt_[k] != kNotHeld && releaseAt_[k] <= upTo)
                    noteOff(frame, k);
    }
    /// Ends every held key at `frame`.
    void releaseHeld(int frame) noexcept { release(kNoRelease, frame); }

    /// Held keys, plus one while events wait for the plugin: what keeps a
    /// removed channel's state alive until its plugin has heard the note-offs.
    [[nodiscard]] int active() const noexcept { return held_ + (count_ > 0 ? 1 : 0); }
    [[nodiscard]] int held() const noexcept { return held_; }

    /// The events of the block so far, in frame order.
    [[nodiscard]] std::span<const NoteEvent> events() const noexcept { return {events_.data(), count_}; }
    /// After the plugin processed the block.
    void clearEvents() noexcept { count_ = 0; }
    /// Forgets every key and event, sending nothing (the plugin is reset or gone).
    void reset() noexcept
    {
        releaseAt_.fill(kNotHeld);
        held_ = 0;
        count_ = 0;
    }

private:
    static constexpr int kKeys = 128;
    static constexpr std::int64_t kNotHeld = -1;

    void noteOff(int frame, std::size_t key) noexcept
    {
        releaseAt_[key] = kNotHeld;
        --held_;
        // Always fits: noteOn() keeps a slot for the off of every held key.
        if (count_ < kMaxEvents)
            events_[count_++] = NoteEvent{frame, NoteEvent::Type::Off, static_cast<std::int16_t>(key), 0.0f};
    }

    std::array<std::int64_t, kKeys> releaseAt_ = filled();
    int held_ = 0;
    std::array<NoteEvent, kMaxEvents> events_{};
    std::size_t count_ = 0;

    static constexpr std::array<std::int64_t, kKeys> filled() noexcept
    {
        std::array<std::int64_t, kKeys> a{};
        a.fill(kNotHeld);
        return a;
    }
};

} // namespace vloom
