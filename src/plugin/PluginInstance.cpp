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
    makesNotes_.store(processor_->makesNotes(), std::memory_order_relaxed);
    const int latency = std::clamp(processor_->latencyFrames(), 0, kMaxLatencyFrames);
    latency_.store(latency, std::memory_order_relaxed);
    bypassLine_.resize(std::min(latency + kBypassHeadroom, kMaxLatencyFrames));
    bypassedLast_ = false;
    active_.store(true, std::memory_order_release); // after maxBlockFrames_, for the audio thread
    return true;
}

bool PluginInstance::updateLatency()
{
    if (!active_.load(std::memory_order_acquire))
        return false;
    const int latency = std::clamp(processor_->latencyFrames(), 0, kMaxLatencyFrames);
    return latency_.exchange(latency, std::memory_order_relaxed) != latency;
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
    // An open editor follows. A processor that takes the value itself (the
    // built-ins) needs no queue: it has it even while no audio runs.
    if (processor_->parameterChangedByHost(id, value))
        return true;
    return parameters_.push(ParameterEvent{0, id, value});
}

void PluginInstance::process(float* left, float* right, int numFrames, const TransportInfo& transport,
    std::span<const NoteEvent> notes) noexcept
{
    process(left, right, numFrames, transport, notes, Io{});
}

void PluginInstance::process(float* left, float* right, int numFrames, const TransportInfo& transport,
    std::span<const NoteEvent> notes, const Io& io) noexcept
{
    if (io.notesOutCount != nullptr)
        *io.notesOutCount = 0;
    const std::size_t extras = std::min(io.extraOutputs.size(), 2 * kMaxExtraOutputs);
    const auto silenceExtras = [&] {
        for (std::size_t i = 0; i < extras; ++i)
            if (io.extraOutputs[i] != nullptr && numFrames > 0)
                std::fill_n(io.extraOutputs[i], static_cast<std::size_t>(numFrames), 0.0f);
    };
    // Changes queued since the last block, at its first frame; the latest
    // value per parameter. While inactive or bypassed they are kept (not
    // dropped) and reach the processor with the first block it processes:
    // drained first, so the queue never fills up meanwhile.
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
    if (!active_.load(std::memory_order_acquire) || numFrames <= 0) {
        silenceExtras();
        return;
    }
    if (bypassed_.load(std::memory_order_relaxed)) {
        silenceExtras();
        NoteEvent dropped; // notes played by hand meanwhile: not kept for later
        while (liveNotes_.pop(dropped)) { }
        ControlEvent droppedControl;
        while (liveControls_.pop(droppedControl)) { }
        // Notes ending while bypassed never reach the processor, and the
        // host forgets them: end everything it holds as bypass starts, so
        // nothing hangs once it processes again.
        if (!bypassedLast_) {
            processor_->reset();
            bypassLine_.clear();
        }
        bypassedLast_ = true;
        // The dry sound, as late as the processed one would be.
        float* dry[2] = {left, right};
        bypassLine_.process(
            dry, 2, numFrames, std::min(latency_.load(std::memory_order_relaxed), bypassLine_.maxDelay()));
        capture(left, right, numFrames);
        return;
    }
    bypassedLast_ = false;
    float* channels[2] = {left, right};
    if (notes.size() > kMaxNotes)
        notes = notes.first(kMaxNotes);
    std::size_t nextNote = 0;
    // Controls sent since the last block: at its first frame.
    std::size_t controlCount = 0;
    for (ControlEvent control; controlCount < blockControls_.size() && liveControls_.pop(control);) {
        control.frame = 0;
        blockControls_[controlCount++] = control;
    }
    const bool sidechain = io.sidechainLeft != nullptr && io.sidechainRight != nullptr;
    std::array<float*, 2 * kMaxExtraOutputs> extraChunk{};
    std::size_t notesMade = 0;
    for (int offset = 0; offset < numFrames; offset += maxBlockFrames_) {
        const int frames = std::min(maxBlockFrames_, numFrames - offset);
        const bool last = offset + frames >= numFrames;
        // This chunk's notes, at frames relative to it (out of range: clamped).
        std::size_t count = 0;
        if (offset == 0) {
            // Notes played by hand since the last block: at its first frame.
            NoteEvent live;
            while (count < kMaxNotes && liveNotes_.pop(live)) {
                live.frame = 0;
                chunkNotes_[count++] = live;
            }
        }
        while (nextNote < notes.size()) {
            const int frame = std::clamp(notes[nextNote].frame, 0, numFrames - 1);
            if (frame >= offset + frames && !last)
                break;
            NoteEvent note = notes[nextNote++];
            note.frame = std::clamp(frame - offset, 0, frames - 1);
            if (count < kMaxNotes) // more than fit with the ones played by hand: dropped
                chunkNotes_[count++] = note;
        }
        channels[0] = left + offset;
        channels[1] = right + offset;
        ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numFrames = frames;
        if (offset == 0) {
            context.parameters = std::span<const ParameterEvent>(pending_.data(), pendingCount_);
            context.controls = std::span<const ControlEvent>(blockControls_.data(), controlCount);
        }
        context.notes = std::span<const NoteEvent>(chunkNotes_.data(), count);
        context.transport = transport;
        const float* const sidechainChunk[2] = {sidechain ? io.sidechainLeft + offset : nullptr,
            sidechain ? io.sidechainRight + offset : nullptr};
        if (sidechain)
            context.sidechain = sidechainChunk;
        for (std::size_t i = 0; i < extras; ++i) {
            extraChunk[i] = io.extraOutputs[i] != nullptr ? io.extraOutputs[i] + offset : nullptr;
            if (extraChunk[i] != nullptr)
                std::fill_n(extraChunk[i], static_cast<std::size_t>(frames), 0.0f);
        }
        // Only whole pairs with both buffers there.
        std::size_t usable = 0;
        while (usable + 1 < extras && extraChunk[usable] != nullptr && extraChunk[usable + 1] != nullptr)
            usable += 2;
        context.extraOutputs = std::span<float* const>(extraChunk.data(), usable);
        int made = 0;
        if (io.notesOutCount != nullptr && notesMade < io.notesOut.size()) {
            context.notesOut = io.notesOut.subspan(notesMade);
            context.notesOutCount = &made;
        }
        processor_->process(context);
        // The notes it made, at frames of the whole block.
        made = std::clamp(made, 0, static_cast<int>(context.notesOut.size()));
        for (int i = 0; i < made; ++i) {
            NoteEvent& note = io.notesOut[notesMade + static_cast<std::size_t>(i)];
            note.frame = std::clamp(note.frame, 0, frames - 1) + offset;
        }
        notesMade += static_cast<std::size_t>(made);
    }
    // In frame order, whatever order the processor made them in (the next
    // processor needs them so). Few, and nearly sorted: an insertion sort.
    for (std::size_t i = 1; i < notesMade; ++i)
        for (std::size_t k = i; k > 0 && io.notesOut[k].frame < io.notesOut[k - 1].frame; --k)
            std::swap(io.notesOut[k], io.notesOut[k - 1]);
    if (io.notesOutCount != nullptr)
        *io.notesOutCount = static_cast<int>(notesMade);
    pendingCount_ = 0;
    capture(left, right, numFrames);
}

bool PluginInstance::playNote(int key, float velocity)
{
    if (!std::isfinite(velocity))
        return false;
    return liveNotes_.push(NoteEvent{0, NoteEvent::Type::On,
        static_cast<std::int16_t>(std::clamp(key, 0, 127)), std::clamp(velocity, 0.0f, 1.0f)});
}

bool PluginInstance::sendControl(ControlEvent::Type type, int number, float value)
{
    if (!std::isfinite(value))
        return false;
    const float low = type == ControlEvent::Type::PitchBend ? -1.0f : 0.0f;
    return liveControls_.push(ControlEvent{
        0, type, static_cast<std::int16_t>(std::clamp(number, 0, 127)), std::clamp(value, low, 1.0f)});
}

bool PluginInstance::stopNote(int key)
{
    return liveNotes_.push(
        NoteEvent{0, NoteEvent::Type::Off, static_cast<std::int16_t>(std::clamp(key, 0, 127)), 0.0f});
}

void PluginInstance::setScopeEnabled(bool enabled)
{
    if (enabled == scopeOn_.load(std::memory_order_relaxed))
        return;
    if (enabled) {
        if (scope_ == nullptr)
            scope_ = std::make_unique<std::atomic<float>[]>(kScopeFrames); // zeroed
        // Start empty: what an earlier run left is not this one's sound. (A
        // block still being written from that run may undo this; the scope
        // then shows a moment of the older sound, nothing worse.)
        scopeWritten_.store(0, std::memory_order_relaxed);
    }
    scopeOn_.store(enabled, std::memory_order_release); // after scope_, for the audio thread
}

void PluginInstance::capture(const float* left, const float* right, int numFrames) noexcept
{
    if (!scopeOn_.load(std::memory_order_acquire))
        return;
    std::uint64_t written = scopeWritten_.load(std::memory_order_relaxed);
    for (int i = 0; i < numFrames; ++i, ++written)
        scope_[written % kScopeFrames].store(0.5f * (left[i] + right[i]), std::memory_order_relaxed);
    scopeWritten_.store(written, std::memory_order_release);
}

std::size_t PluginInstance::readScope(std::span<float> out) const noexcept
{
    if (!scopeOn_.load(std::memory_order_acquire))
        return 0;
    const std::uint64_t written = scopeWritten_.load(std::memory_order_acquire);
    const std::size_t count
        = static_cast<std::size_t>(std::min<std::uint64_t>({written, out.size(), kScopeFrames}));
    const std::uint64_t first = written - count;
    for (std::size_t i = 0; i < count; ++i)
        out[i] = scope_[(first + i) % kScopeFrames].load(std::memory_order_relaxed);
    return count;
}

void PluginInstance::reset() noexcept
{
    if (active_.load(std::memory_order_acquire))
        processor_->reset();
}

} // namespace vloom
