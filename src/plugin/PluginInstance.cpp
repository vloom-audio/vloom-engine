// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/PluginInstance.hpp"

#include <algorithm>
#include <cmath>

namespace vloom {

PluginInstance::PluginInstance(std::unique_ptr<AudioProcessor> processor)
    : processor_(std::move(processor))
{
}

PluginInstance::~PluginInstance() { deactivate(); }

bool PluginInstance::activate(double sampleRate, int maxBlockFrames)
{
    deactivate();
    if (processor_ == nullptr || !std::isfinite(sampleRate) || sampleRate <= 0.0 || maxBlockFrames <= 0)
        return false;
    if (!processor_->activate(sampleRate, maxBlockFrames))
        return false;
    maxBlockFrames_ = maxBlockFrames;
    active_.store(true, std::memory_order_release); // after maxBlockFrames_, for the audio thread
    return true;
}

void PluginInstance::deactivate()
{
    if (!active_.exchange(false, std::memory_order_acq_rel))
        return;
    if (processor_ != nullptr)
        processor_->deactivate();
}

bool PluginInstance::setParameter(std::uint32_t id, double value)
{
    if (!std::isfinite(value))
        return false;
    return parameters_.push(ParameterEvent{0, id, value});
}

void PluginInstance::process(float* left, float* right, int numFrames, const TransportInfo& transport,
    std::span<const NoteEvent> notes) noexcept
{
    if (!active_.load(std::memory_order_acquire) || numFrames <= 0)
        return;
    // Changes queued since the last block, at its first frame; the latest
    // value per parameter. While bypassed they are kept (not dropped) and
    // reach the processor with the first block it processes again.
    ParameterEvent event;
    while (parameters_.pop(event)) {
        auto* end = pending_.data() + pendingCount_;
        auto* same
            = std::find_if(pending_.data(), end, [&](const ParameterEvent& e) { return e.id == event.id; });
        if (same != end)
            same->value = event.value;
        else if (pendingCount_ < pending_.size())
            pending_[pendingCount_++] = event;
    }
    if (bypassed_.load(std::memory_order_relaxed)) {
        // Notes ending while bypassed never reach the processor, and the
        // host forgets them: end everything it holds as bypass starts, so
        // nothing hangs once it processes again.
        if (!bypassedLast_)
            processor_->reset();
        bypassedLast_ = true;
        return;
    }
    bypassedLast_ = false;
    float* channels[2] = {left, right};
    if (notes.size() > kMaxNotes)
        notes = notes.first(kMaxNotes);
    std::size_t nextNote = 0;
    for (int offset = 0; offset < numFrames; offset += maxBlockFrames_) {
        const int frames = std::min(maxBlockFrames_, numFrames - offset);
        const bool last = offset + frames >= numFrames;
        // This chunk's notes, at frames relative to it (out of range: clamped).
        std::size_t count = 0;
        while (nextNote < notes.size()) {
            const int frame = std::clamp(notes[nextNote].frame, 0, numFrames - 1);
            if (frame >= offset + frames && !last)
                break;
            NoteEvent note = notes[nextNote++];
            note.frame = std::clamp(frame - offset, 0, frames - 1);
            chunkNotes_[count++] = note;
        }
        channels[0] = left + offset;
        channels[1] = right + offset;
        ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numFrames = frames;
        if (offset == 0)
            context.parameters = std::span<const ParameterEvent>(pending_.data(), pendingCount_);
        context.notes = std::span<const NoteEvent>(chunkNotes_.data(), count);
        context.transport = transport;
        processor_->process(context);
    }
    pendingCount_ = 0;
}

void PluginInstance::reset() noexcept
{
    if (active_.load(std::memory_order_acquire))
        processor_->reset();
}

} // namespace vloom
