// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/render/WavWriter.hpp"
#include "vloom/engine/sequencer/SequencerState.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vloom {

/// How exportSong() renders.
struct SongExportOptions {
    WavFormat format = WavFormat::Pcm24;
    bool dither = true; ///< integer formats: TPDF dither (WavFileWriter)
    int channels = 2; ///< 1 (mono mix-down) or 2
    double tempo = 120.0; ///< BPM, clamped like Engine::setTempo()
    float masterGain = 1.0f;
    float masterPan = 0.0f;
    double masterPitchCents = 0.0;
    /// The range to export, in ticks. endTicks < 0: to the song's end
    /// (songEndTicks()). After the end the render goes on until the sound
    /// has died away (reverb-like tails, releases), at most maxTailSeconds.
    std::int64_t startTicks = 0;
    std::int64_t endTicks = -1;
    double maxTailSeconds = 10.0;
    /// Below this the tail counts as silent (about -100 dBFS); 50 ms of it ends the tail.
    float silenceThreshold = 1e-5f;
    int blockSize = 512;
};

struct SongExportResult {
    bool ok = false;
    bool cancelled = false; ///< the progress callback stopped it; no file was written
    std::string error; ///< why it failed
    std::uint64_t frames = 0; ///< written, tail included
};

/// Called after each rendered block with the share done, 0 .. 1 (the tail
/// is estimated). Return false to cancel. Runs on the exporting thread.
using SongExportProgress = std::function<bool(double done)>;

/// Where `song` ends, in ticks: the end of its last pattern clip or audio
/// clip (an audio clip's length depends on `tempo`: it plays at the
/// sample's own speed). Pattern mode (song.song false): one pass of the
/// pattern. 0 for an empty song.
[[nodiscard]] std::int64_t songEndTicks(const SequencerState& song, double tempo);

/// A copy of `state` with new ChannelVoices of the same polyphony for every
/// channel (samples stay shared): what an Engine other than the live one
/// must use (ChannelVoices belong to one Engine).
[[nodiscard]] std::unique_ptr<SequencerState> copyWithOwnVoices(const SequencerState& state);

/// Renders `song` (through `mixer`, if given) into a WAV file at `path`
/// (UTF-8), faster than real time, with an Engine of its own: the live
/// engine and its device are not touched, so it may run on a background
/// thread while the app plays. Samples in `song` must be at `sampleRate`
/// (load them for the export rate first). Blocks until done, cancelled or
/// failed; a file is written only on success.
SongExportResult exportSong(const std::string& path, const SequencerState& song, const MixerState* mixer,
    double sampleRate, const SongExportOptions& options = {}, const SongExportProgress& progress = {});

} // namespace vloom
