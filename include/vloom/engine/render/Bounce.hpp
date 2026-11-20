// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/render/SongExport.hpp"
#include "vloom/engine/render/WavWriter.hpp"
#include "vloom/engine/sequencer/SequencerState.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vloom {

/// What a bounce (bouncePattern(), bounceRange()) renders the channels
/// through (ADR 0018).
enum class BounceMix : std::uint8_t {
    /// Each channel's own sound (its instrument, volume and pan), summed with
    /// no mixer insert and no master effect: for an audio clip that will play
    /// through the same channel, and so the same insert, again.
    Dry,
    /// Through the channels' inserts as in the song (effects, volume, pan,
    /// routes and sends) into the master bus, before the master's effects,
    /// volume and pan: for an audio clip that will play into the master.
    Inserts,
    /// Through the whole mixer, the master's effects, masterGain and
    /// masterPan included: what an export of that stretch would give.
    Master,
};

/// How a bounce renders. Stereo, at the rate it is given (the project's).
struct BounceOptions {
    /// Channels heard (indexes into SequencerState::channels); the others
    /// are muted for the bounce. Empty: every channel.
    std::vector<int> channels{};
    BounceMix mix = BounceMix::Inserts;
    double tempo = 120.0; ///< BPM, clamped like Engine::setTempo()
    /// As the channel rack plays it (Engine::setMasterPitch()), which an
    /// audio clip is not transposed by: the bounce sounds as the pattern did.
    double masterPitchCents = 0.0;
    float masterGain = 1.0f; ///< BounceMix::Master only
    float masterPan = 0.0f; ///< BounceMix::Master only
    /// After the pattern's (or range's) end the render goes on until the
    /// sound has died away, at most this long, as in an export; 0: exactly
    /// the pattern's length.
    double maxTailSeconds = 10.0;
    float silenceThreshold = 1e-5f; ///< as SongExportOptions::silenceThreshold
    int blockSize = 512;
    /// A stereo WAV file to write (UTF-8), in `format`; written only on
    /// success. Empty: the audio goes into BounceResult::audio instead, at
    /// most kMaxInMemoryBounceSeconds of it: the body plus maxTailSeconds
    /// must fit, checked before rendering; longer fails with an error, like
    /// running out of memory does.
    std::string path{};
    WavFormat format = WavFormat::Float32;
    bool dither = true; ///< integer formats: TPDF dither
};

struct BounceResult {
    bool ok = false;
    bool cancelled = false; ///< the progress callback stopped it; no file was written
    std::string error; ///< why it failed
    /// Interleaved stereo at the bounce's rate, tail included; empty when
    /// written to a file (BounceOptions::path).
    std::vector<float> audio;
    std::uint64_t frames = 0; ///< rendered, tail included
    std::uint64_t bodyFrames = 0; ///< the pattern's (or range's) own length, without the tail
    std::int64_t lengthTicks = 0; ///< the same in ticks: how long a clip of it is in the playlist
};

/// bouncePattern()'s `pattern` for the channel rack's own pattern (pattern
/// mode: ChannelState::steps, notes and controls, SequencerState::stepCount
/// steps long) instead of one of SequencerState::patterns.
inline constexpr int kChannelRackPattern = -1;

/// The longest bounce kept in memory (BounceOptions::path empty), in
/// seconds at the render's sample rate: 30 minutes, about 660 MB of stereo
/// floats at 48 kHz. Longer ranges bounce to a file.
inline constexpr double kMaxInMemoryBounceSeconds = 30.0 * 60.0;

/// How long pattern `pattern` of `state` is in ticks: its stepCount steps
/// (SongPattern::stepCount; 0: its longest row of steps; still 0: the
/// state's stepCount) of state.ticksPerStep. kChannelRackPattern: the
/// state's stepCount steps. 0 if there is no such pattern.
[[nodiscard]] std::int64_t patternLengthTicks(const SequencerState& state, int pattern);

/// Renders one pass of pattern `pattern` (or kChannelRackPattern) of
/// `state`, from its first tick, with `state`'s channels and `mixer` (may be
/// null: no inserts' effects), whatever the playlist holds: what a pattern
/// clip of it would play from the clip's start, so the app can replace the
/// clip with an audio clip. Notes still sounding at the end ring into the
/// tail. Samples must be at `sampleRate`.
///
/// Like exportSong() it runs on the calling thread (not the audio thread)
/// with an Engine and voices of its own, so the live engine is not touched.
/// Plugin instances in `state` and `mixer` are shared with the live engine,
/// though: stop the device meanwhile when there are any, as for an export.
BounceResult bouncePattern(const SequencerState& state, int pattern, const MixerState* mixer,
    double sampleRate, const BounceOptions& options = {}, const SongExportProgress& progress = {});

/// The same for part of the song (song mode): [startTicks, endTicks) of
/// `song` as it plays, audio clips included, with the channels and mix of
/// `options`.
BounceResult bounceRange(const SequencerState& song, std::int64_t startTicks, std::int64_t endTicks,
    const MixerState* mixer, double sampleRate, const BounceOptions& options = {},
    const SongExportProgress& progress = {});

} // namespace vloom
