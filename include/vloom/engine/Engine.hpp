// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/core/SpscRing.hpp"
#include "vloom/engine/dsp/DelayLine.hpp"
#include "vloom/engine/dsp/Metronome.hpp"
#include "vloom/engine/dsp/Sampler.hpp"
#include "vloom/engine/mixer/MixerGraph.hpp"
#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/sequencer/MidiTake.hpp"
#include "vloom/engine/sequencer/StepSequencer.hpp"
#include "vloom/engine/time/Transport.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace vloom {

/// Where recorded audio (Engine::takeRecordedAudio()) sits in the song
/// (ADR 0016). The frames recorded form one stream counted from the first
/// recording; a mark says what the engine's position was at one of them.
struct AudioRecordMark {
    enum class Kind : std::uint8_t {
        /// Recording began here (the song started, or recording was
        /// switched on while it played): a new take.
        Start,
        /// The song jumped here (a loop going back, a locate, recording
        /// going on after frames were lost), or its tempo changed.
        Jump,
        /// Punch recording (Engine::setPunch()): a stretch kept inside the
        /// punch range starts here. Unlike the others, `ticks` is where the
        /// player heard this very frame: the shift is already taken off, and
        /// the mark is not moved by it (ADR 0017).
        Punch,
    };
    Kind kind = Kind::Start;
    std::int64_t frame = 0; ///< in the recorded stream
    double ticks = 0.0; ///< the engine's position at that frame
    double ticksPerFrame = 0.0; ///< the tempo then
    std::int32_t pass = 0; ///< loop passes since the take started
    /// Start only: frames from when the engine renders a moment to when its
    /// sound, played along with, comes back in: the device's round trip
    /// (Engine::setAudioRecordLatency()), the plugins' latency and the input
    /// waiting in the engine. The audio is that much behind the song.
    std::int32_t shiftFrames = 0;
};

/// The audio engine.
///
/// Threading contract
///  - prepare() is called from a non-real-time thread while audio is stopped.
///  - process() is called only from the audio thread. It never allocates,
///    locks, or does I/O.
///  - Control calls (play, stop, setTempo, ...) may be made from ONE control
///    thread (usually the UI thread). They are queued and applied at the start
///    of the next audio block.
///  - Status getters may be called from any thread.
class Engine {
public:
    /// Allocates the mixer's buses (about 4 MB), so process() never has to.
    Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    static constexpr double kMinSampleRate = 8000.0;
    static constexpr double kMaxSampleRate = 768000.0;

    /// `sampleRate` is clamped to kMinSampleRate .. kMaxSampleRate; a
    /// non-finite one means 48 kHz. Silences every voice (the preview too):
    /// samples loaded for another rate must be replaced by the host, with a
    /// new setSequencerState(). Commands queued meanwhile are applied.
    ///
    /// ChannelVoices belong to one Engine: an offline render (export) while
    /// the device plays needs its own Engine and its own ChannelVoices. Keep
    /// the Engine alive as long as any ChannelVoices it played.
    void prepare(double sampleRate, int maxBlockSize);

    /// Renders numFrames of non-interleaved audio into `out`. Blocks longer
    /// than kMaxBlockFrames are rendered in parts.
    void process(float* const* out, int numChannels, int numFrames) noexcept;

    // ---- control (one control thread) — return false if the queue is full
    bool play();
    bool stop();
    bool locate(double ticks);
    bool setTempo(double bpm);
    bool setMetronomeEnabled(bool enabled);
    /// Linear output gain, clamped to [0, kMaxMasterGain]; NaN is ignored.
    /// Changes ramp over one block, so moving a knob does not click.
    bool setMasterGain(float gain);
    /// Stereo balance of the output, -1 (left) .. 0 (centre) .. +1 (right),
    /// clamped; NaN is ignored. Ramps like the gain.
    bool setMasterPan(float pan);

    /// Transposes everything the channel rack plays (steps and notes, not
    /// audio clips or the preview) by `cents`, clamped to
    /// ±kMaxMasterPitchCents; sounding notes follow. NaN is ignored.
    bool setMasterPitch(double cents);

    /// Loops [startTicks, endTicks) while playing: on reaching endTicks the
    /// position jumps back to startTicks on that exact sample, and events
    /// there fire again (song-mode audio clips under way resume part-way,
    /// held notes are released, as for locate()). Playing from at or past
    /// endTicks goes on without looping. A loop shorter than kMinLoopTicks,
    /// or non-finite values, turn looping off.
    bool setLoop(double startTicks, double endTicks);
    /// Turns looping off.
    bool clearLoop() { return setLoop(0.0, 0.0); }
    /// Stops the transport on reaching `ticks`, on that exact sample (e.g.
    /// the song's end): sounding notes fade out as on stop(). <= 0 or
    /// non-finite: never. A loop that ends first wins.
    bool setStopAt(double ticks);
    static constexpr double kMinLoopTicks = kPpq / 16.0; ///< a 64th note

    // ---- time signature, count-in and punch recording (ADR 0017)
    static constexpr int kMaxBeatsPerBar = 32;
    static constexpr int kMaxCountInBars = 4;
    /// Beats (quarter notes, kPpq ticks) to a bar, clamped to 1 ..
    /// kMaxBeatsPerBar: where the metronome accents, how long a count-in bar
    /// is, and what hosted plugins are told. 4 by default.
    bool setBeatsPerBar(int beats);
    /// Bars of count-in, clamped to 0 (off) .. kMaxCountInBars. When play()
    /// starts the song from stop while a recording is armed (setRecording()
    /// or setAudioRecording() on), the song first stands still at its
    /// position for `bars` bars at the current tempo and time signature
    /// while the metronome counts them, then starts there on the exact
    /// sample the next bar would begin. Nothing is recorded during it. Stop
    /// cancels it; a locate meanwhile moves where the song will start.
    bool setCountIn(int bars);
    /// True (the default): the count-in clicks even when the metronome is
    /// off (setMetronomeEnabled(false)). False: it clicks only when the
    /// metronome is on, and is a silent wait otherwise.
    bool setCountInClick(bool always);
    /// Records only inside [startTicks, endTicks) of the song as the player
    /// hears it (latency taken off): audio and MIDI played outside it is
    /// still heard (monitored, played live) but not recorded. Independent of
    /// the loop; in a loop each pass records its part of the range. Shorter
    /// than kMinLoopTicks, or non-finite values: punch off (record
    /// everything). Range changes apply at once; switching punch on or off
    /// applies to the audio take from its next start.
    bool setPunch(double startTicks, double endTicks);
    bool clearPunch() { return setPunch(0.0, 0.0); }

    static constexpr float kMaxMasterGain = 2.0f;
    static constexpr double kMaxMasterPitchCents = 1200.0;

    /// Replaces the channel rack (channels, samples, steps) the audio thread
    /// plays, from the next block on. Ringing notes are not cut: keep each
    /// channel's ChannelVoices across states (docs/adr/0005).
    void setSequencerState(std::unique_ptr<SequencerState> state);
    /// Frees states the audio thread has finished with. Call it regularly
    /// from the control thread (a UI timer); setSequencerState() does too.
    void collectGarbage();
    /// Replaces the mixer inserts (volume, pan, effects, routing; mute and
    /// solo folded into the gain by the host), from the next block on;
    /// volume and send changes ramp. Works out the routes' order and the
    /// delays lining up paths with plugin latency (ADR 0007). Latency that
    /// changes later is picked up by updatePluginLatency().
    void setMixerState(std::unique_ptr<const MixerState> state);
    /// Reads the latency of every effect in the published mixer state again,
    /// on inserts and on the master (PluginInstance::updateLatency()). If one changed, the state is
    /// published again, so the paths are lined up anew; returns true then.
    /// Control thread, often: from the same UI timer as
    /// AudioProcessor::idle().
    bool updatePluginLatency();
    /// Plays `sample` once from its start on the master bus (after the master
    /// volume), whether or not the transport plays: a browser preview. A new
    /// preview replaces the one sounding, which fades out; null stops it.
    /// The sample must be at the engine's rate (sample/Resampler). Stop
    /// (stop()) silences it too.
    void previewSample(std::shared_ptr<const SampleBuffer> sample, float gain = 1.0f);

    // ---- audio input, monitoring and recording it (ADR 0016)
    /// Input frames that can wait for the audio thread (1.4 s at 48 kHz).
    static constexpr std::size_t kInputRingFrames = 65536;
    /// Hands the engine `numFrames` of input at the engine's rate: `left`,
    /// and `right` or null for a mono input. From one thread: the input's
    /// (AudioInput's callback). Real-time safe. Input that does not fit is
    /// dropped. The audio thread takes one block of it per block it
    /// renders; it keeps no more than about four blocks waiting, dropping
    /// the oldest, so a fast input clock cannot make it lag more and more.
    void pushAudioInput(const float* left, const float* right, int numFrames) noexcept;
    /// Plays the input into mixer insert `insert` (0 = the master; -1 or
    /// out of range = not at all) at linear `gain` (0 .. 4; changes ramp):
    /// hearing what is being played or recorded. It goes through the insert
    /// like a channel's sound, so it is as late as the plugins there make it.
    bool setInputMonitor(int insert, float gain = 1.0f);
    /// While on and the song plays, the input is recorded, for
    /// takeRecordedAudio() and takeAudioMarks() (AudioTake does both): only
    /// inside the punch range if one is set (setPunch()), and not during a
    /// count-in (setCountIn()).
    bool setAudioRecording(bool enabled);
    /// The device's round trip in frames: from rendering a moment to its
    /// sound, played along with, coming back as input (the output's latency
    /// plus the input's). Clamped to 0 .. kMaxRecordLatencyFrames.
    bool setAudioRecordLatency(int frames);
    static constexpr int kMaxRecordLatencyFrames = 192000;
    /// Recorded frames that can wait (10.9 s at 48 kHz); when the reader
    /// falls further behind, blocks are lost (recordedAudioLost()).
    static constexpr std::size_t kRecordRingFrames = 524288;
    /// Takes up to out.size() / 2 recorded frames, interleaved stereo,
    /// oldest first; returns how many frames. Control thread (one reader).
    std::size_t takeRecordedAudio(std::span<float> out) noexcept;
    /// Takes the marks of the recorded stream, oldest first; returns how many.
    std::size_t takeAudioMarks(std::span<AudioRecordMark> out) noexcept;

    // ---- MIDI played live, and recording it (ADR 0015)
    /// Live messages that can wait for the audio thread; more are refused.
    static constexpr std::size_t kMaxLiveMidi = 1023;
    /// Plays `message` live on channel `channel` (an index into
    /// SequencerState::channels) with the next block, whether or not the song
    /// plays: a MIDI keyboard (from MidiInput's callback), an on-screen one.
    /// See StepSequencer::playLive(). While recording and the song plays, it
    /// is recorded too. Any thread, several at once: the callers take a short
    /// lock among themselves, never one the audio thread waits for. False if
    /// too many wait.
    bool playMidi(int channel, const MidiMessage& message);
    /// Ends every note played live, on every channel (all notes off on each
    /// MIDI channel): when a keyboard is closed or goes away with keys down.
    /// Any thread, like playMidi(). False if the queue is full.
    bool releaseLiveNotes();
    /// While on and the song plays, what playMidi() plays is recorded, for
    /// takeRecordedMidi(). Switching it on counts the loop's passes from 0.
    bool setRecording(bool enabled);
    /// How many frames after the engine renders a moment the player hears it
    /// (the output device's latency, and the input's): recorded messages are
    /// placed that much earlier in the song, and latencyFrames() earlier
    /// still. Clamped to 0 .. kMaxRecordLatency.
    bool setRecordLatency(int frames);
    static constexpr int kMaxRecordLatency = 192000;
    /// Recorded messages that can wait for takeRecordedMidi(); further ones are lost.
    static constexpr std::size_t kMaxRecordedMidi = 4095;
    /// Takes what was recorded since the last call into `out`, oldest first;
    /// returns how many. Call it often (a UI timer) and feed a MidiTake.
    /// Control thread (one reader).
    std::size_t takeRecordedMidi(std::span<RecordedMidi> out) noexcept;

    // ---- status (any thread)
    /// True while the song plays; false during a count-in (isCountingIn()).
    [[nodiscard]] bool isPlaying() const noexcept { return playing_.load(std::memory_order_relaxed); }
    /// True while a count-in (setCountIn()) counts: the song starts after it.
    [[nodiscard]] bool isCountingIn() const noexcept
    {
        return countInBeatsLeft_.load(std::memory_order_relaxed) > 0;
    }
    /// Beats of the count-in still to come, the one sounding included (e.g.
    /// 8 .. 1 for two bars of 4/4); 0 when not counting in.
    [[nodiscard]] int countInBeatsLeft() const noexcept
    {
        return countInBeatsLeft_.load(std::memory_order_relaxed);
    }
    /// Bars of the count-in still to come, the one sounding included; 0
    /// when not counting in.
    [[nodiscard]] int countInBarsLeft() const noexcept
    {
        return countInBarsLeft_.load(std::memory_order_relaxed);
    }
    /// True while setRecording() is on (whether or not the song plays).
    [[nodiscard]] bool isRecording() const noexcept
    {
        return recordingStatus_.load(std::memory_order_relaxed);
    }
    /// True while setAudioRecording() is on (whether or not the song plays).
    [[nodiscard]] bool isRecordingAudio() const noexcept
    {
        return audioRecordingStatus_.load(std::memory_order_relaxed);
    }
    /// Blocks that found too little input waiting since the input started
    /// coming (the input's clock is slower, or it stalled).
    [[nodiscard]] std::uint64_t inputDropouts() const noexcept
    {
        return inputDropouts_.load(std::memory_order_relaxed);
    }
    /// Recorded frames lost because they were not taken in time.
    [[nodiscard]] std::uint64_t recordedAudioLost() const noexcept
    {
        return recordLost_.load(std::memory_order_relaxed);
    }
    /// True while a preview (previewSample()) is sounding, its fade included.
    [[nodiscard]] bool isPreviewPlaying() const noexcept
    {
        return previewPlaying_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double positionTicks() const noexcept { return position_.load(std::memory_order_relaxed); }
    [[nodiscard]] double tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }
    [[nodiscard]] double sampleRate() const noexcept
    {
        return sampleRateStatus_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int beatsPerBar() const noexcept
    {
        return beatsPerBarStatus_.load(std::memory_order_relaxed);
    }
    /// Frames the mixer delays the music by to line up plugin latency (ADR
    /// 0007): what is heard comes this late compared with positionTicks().
    /// The metronome is delayed as much; the preview is not.
    [[nodiscard]] int latencyFrames() const noexcept { return latency_.load(std::memory_order_relaxed); }
    /// Smoothed share of each block's real-time budget that process() used:
    /// 0 = idle, 1 = the whole block (audio starts to drop out above that).
    [[nodiscard]] float cpuLoad() const noexcept { return cpuLoad_.load(std::memory_order_relaxed); }
    /// Highest absolute sample on output `channel` (0 = left, 1 = right) since
    /// the last call, after master gain and pan; resets it to 0. For one
    /// meter reader (the UI); other channels read 0.
    [[nodiscard]] float takeOutputPeak(int channel) noexcept;
    /// Like takeOutputPeak() for mixer insert `insert` (1 .. kMaxInserts),
    /// after its volume and pan.
    [[nodiscard]] float takeInsertPeak(int insert, int channel) noexcept;

private:
    struct Command {
        enum class Type {
            Play,
            Stop,
            Locate,
            SetTempo,
            SetMetronome,
            SetMasterGain,
            SetMasterPan,
            SetMasterPitch,
            SetLoop,
            SetStopAt,
            SetInputMonitor,
            SetAudioRecording,
            SetAudioRecordLatency,
            SetRecording,
            SetRecordLatency,
            SetBeatsPerBar,
            SetCountIn,
            SetCountInClick,
            SetPunch
        };
        Type type;
        double value;
        double value2 = 0.0;
    };

    bool send(Command::Type type, double value = 0.0, double value2 = 0.0);
    /// Runs the event scheduler over [spanStart, spanStart + spanFrames) of
    /// the block, the transport standing at spanStart; renders that span.
    void playSpan(int spanStart, int spanFrames) noexcept;
    void applyCommands() noexcept;
    /// Takes this block's input from the ring into input_.
    void readInput(int numFrames) noexcept;
    /// Adds the input into the monitored insert's bus.
    void monitorInput(int numFrames) noexcept;
    /// Writes this block's input to the recording, if it records, along the
    /// spans the block was played in (spans_).
    void recordInput(int numFrames) noexcept;
    /// Writes frames [from, to) of the block's input to the recording ring;
    /// false (and counted lost) if they do not fit.
    bool writeRecorded(int from, int to) noexcept;
    /// Punch recording: keeps the frames [from, to) of `span` that the
    /// player heard inside the punch range.
    struct Span;
    void recordPunched(const Span& span, int from, int to) noexcept;
    /// Plays up to maxFrames of a count-in from spanStart (clicks on its
    /// beats); starts the song if it ends within them. Returns the frames played.
    int playCountIn(int spanStart, int maxFrames) noexcept;
    /// Plays the live MIDI that waits, and records it.
    void playLiveMidi() noexcept;
    void publishStatus() noexcept;
    void applyMasterGain(float* const* out, int numChannels, int numFrames) noexcept;
    [[nodiscard]] float channelGainTarget(int channel, int numChannels) const noexcept;
    void meterOutput(float* const* out, int numChannels, int numFrames) noexcept;
    /// NaN / infinity become silence before the device sees them.
    static void sanitizeOutput(float* const* out, int numChannels, int numFrames) noexcept;
    void measureLoad(double seconds, int numFrames) noexcept;
    void renderSegment(int startFrame, int numFrames) noexcept;
    void processBlock(float* const* out, int numChannels, int numFrames) noexcept;
    /// Inserts (effects, volume, pan, meters) along their routes into the
    /// master bus, then the master bus, the preview and the metronome into
    /// `out`. `transport` as at the block's start, for hosted effects.
    void mixBuses(float* const* out, int numChannels, int numFrames, const TransportInfo& transport) noexcept;
    /// Delays node `node`'s direct input by the graph's compensation, once
    /// per block, before anything is routed into it.
    void delayInput(const MixerGraph& graph, int node, int numFrames) noexcept;
    /// Plays `numFrames` of one insert of `graph` along its routes.
    void mixInsert(
        const MixerGraph& graph, int insert, int numFrames, const TransportInfo& transport) noexcept;
    /// Starts a newly published preview and adds the preview into the master bus.
    void renderPreview(int numFrames) noexcept;

    /// What the preview voice plays; one per previewSample() call.
    struct PreviewState {
        std::shared_ptr<const SampleBuffer> sample; ///< null: stop
        float gain = 1.0f;
    };

    SpscQueue<Command, 256> commands_;
    Transport transport_;
    Metronome metronome_;
    StepSequencer sequencer_;
    SnapshotExchange<MixerGraph> mixer_;
    MixerGraph* lastMixer_ = nullptr; ///< control thread: the graph published last (routes are reused)
    std::unique_ptr<MixerGraph> unityMixer_; ///< what plays before any state is published
    SnapshotExchange<PreviewState> preview_;
    Sampler previewSampler_{1}; ///< audio thread: one voice (plus the sampler's fade slots)
    const PreviewState* previewStarted_ = nullptr; ///< audio thread: the preview last triggered
    /// audio thread: stereo bus per insert, 0 = master bus; kMaxBlockFrames each
    std::vector<float> busMemory_;
    std::array<float*, 2 * (kMaxInserts + 1)> buses_{};
    std::array<bool, kMaxInserts + 1> busUsed_{};
    /// audio thread: per node, what was sent to its sidechain this block
    std::vector<float> sideMemory_;
    std::array<float*, 2 * (kMaxInserts + 1)> sideBuses_{};
    std::array<bool, kMaxInserts + 1> sideUsed_{};
    /// Runs `effects` on `left` / `right` with node `index`'s sidechain, and empties it.
    void processEffects(const std::vector<std::shared_ptr<PluginInstance>>& effects, std::size_t index,
        float* left, float* right, int numFrames, const TransportInfo& transport,
        const MixerGraph* graph = nullptr) noexcept;
    std::array<StereoGain, kMaxInserts + 1> insertApplied_{}; ///< audio thread: ramp start per insert
    std::array<bool, kMaxInserts + 1> insertSounding_{}; ///< audio thread: the insert had sound last block
    std::array<std::array<std::atomic<float>, 2>, kMaxInserts + 1> insertPeak_{};
    /// audio thread: per node, its direct input was delayed this block
    std::array<bool, kMaxInserts + 1> inputDelayed_{};
    /// audio thread: an insert after its volume and pan; a route's copy of
    /// it; the metronome (mono); kMaxBlockFrames each
    std::vector<float> scratchMemory_;
    std::array<float*, 2> post_{};
    std::array<float*, 2> routed_{};
    float* click_ = nullptr;
    std::array<float*, 2> side_{}; ///< a sidechain delayed for one effect
    DelayLine clickLine_; ///< audio thread: the metronome, delayed as the music
    bool stopVoices_ = false; ///< audio thread: a Stop command arrived this block
    bool wasRolling_ = false; ///< audio thread: the transport played in the last block
    double expectedTicks_ = 0.0; ///< audio thread: where the last block left the position
    // audio thread: the next event of each stream, carried over between blocks
    std::int64_t nextBeat_ = 0;
    std::int64_t nextStep_ = 0;
    int stepTicksUsed_ = 0; ///< the ticks per step nextStep_ counts in
    std::int64_t noteFrom_ = 0; ///< note starts before this tick are done
    bool metronomeEnabled_ = true;
    // audio thread: the loop region (off when loopEnd_ <= loopStart_) and the stop point (<= 0: none)
    double loopStart_ = 0.0;
    double loopEnd_ = 0.0;
    double stopAt_ = 0.0;
    float masterGainTarget_ = 1.0f; ///< audio thread: latest requested gain
    float masterPanTarget_ = 0.0f; ///< audio thread: latest requested balance
    /// audio thread: per-channel gain at the end of the last block (ramp start)
    std::array<float, 8> channelGain_{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float smoothedLoad_ = 0.0f; ///< audio thread

    // Audio input (ADR 0016).
    SpscRing inputRing_{2 * kInputRingFrames}; ///< interleaved stereo, input thread -> audio thread
    std::vector<float> pushScratch_; ///< input thread: one block interleaved
    std::atomic<bool> inputComing_{false}; ///< the input thread has pushed something
    std::array<float*, 2> input_{}; ///< audio thread: this block's input, kMaxBlockFrames each
    float* inputInterleaved_ = nullptr; ///< audio thread: 2 * kMaxBlockFrames
    int inputWaiting_ = 0; ///< audio thread: frames left in the ring after this block's
    int monitorInsert_ = -1; ///< audio thread
    int monitorActive_ = -1; ///< audio thread: the insert the input went into last block
    float monitorGain_ = 1.0f; ///< audio thread: the target
    float monitorApplied_ = 0.0f; ///< audio thread: where the last block's ramp ended
    SpscRing recordRing_{2 * kRecordRingFrames}; ///< interleaved stereo, audio thread -> control thread
    SpscQueue<AudioRecordMark, 1024> audioMarks_;
    bool audioRecording_ = false; ///< audio thread
    bool audioTake_ = false; ///< audio thread: a take is under way (the song plays and it records)
    bool audioResync_ = false; ///< audio thread: frames were lost, mark where the next ones are
    bool audioStartPending_ = false; ///< audio thread: a take started; no block of it written yet
    int audioRecordLatency_ = 0; ///< audio thread: frames
    std::int64_t recordedFrames_ = 0; ///< audio thread: frames in the recorded stream so far
    std::int32_t audioPass_ = 0; ///< audio thread: loop passes in this take
    double audioExpected_ = 0.0; ///< audio thread: where the position should be at the next block
    double audioTicksPerFrame_ = 0.0; ///< audio thread: the tempo the last mark was made at
    bool audioBlockRolling_ = false; ///< audio thread: the song played at this block's start
    // What the block was played in, for recordInput(): stretches where the
    // position moves linearly, split where the loop jumps back, the song
    // stops or a count-in ends.
    struct Span {
        int from = 0; ///< frame in the block
        int frames = 0;
        double ticks = 0.0; ///< the position at `from`
        double ticksPerFrame = 0.0;
        bool rolling = false; ///< the song played (not stopped, not counting in)
        bool loopsBack = false; ///< the loop jumps back after it
    };
    static constexpr std::size_t kMaxSpans = 1024;
    std::vector<Span> spans_; ///< audio thread: kMaxSpans, allocated with the engine (not on its stack)
    std::size_t spanCount_ = 0; ///< audio thread
    bool spansLost_ = false; ///< audio thread: more spans than fit: the rest of the block is not recorded
    bool audioLoopCarry_ = false; ///< audio thread: the take's last span looped back
    bool audioPunch_ = false; ///< audio thread: the take records only the punch range (set at its start)
    int audioShift_ = 0; ///< audio thread: the take's shift (see AudioRecordMark), set at its start
    int audioSkip_ = 0; ///< audio thread: frames of the take still to leave out (heard during a count-in)
    bool captureOpen_ = false; ///< audio thread: punch: the last frame was kept, a mark is not needed
    std::int64_t captureNext_
        = 0; ///< audio thread: punch: the frame (blockFrame_ count) after the last kept one
    double captureHeard_ = 0.0; ///< audio thread: punch: where the next kept frame was heard if it follows on
    std::int32_t capturePass_ = 0; ///< audio thread: punch: the pass of the last kept frame
    std::int64_t blockFrame_ = 0; ///< audio thread: frames rendered before this block
    std::atomic<bool> audioRecordingStatus_{false};
    std::atomic<std::uint64_t> inputDropouts_{0};
    std::atomic<std::uint64_t> recordLost_{0};
    struct LiveMidi {
        int channel = 0;
        MidiMessage message;
    };
    std::mutex liveMutex_; ///< among the threads calling playMidi()
    SpscQueue<LiveMidi, kMaxLiveMidi + 1> liveMidi_;
    SpscQueue<RecordedMidi, kMaxRecordedMidi + 1> recordedMidi_;
    bool recording_ = false; ///< audio thread
    int recordLatency_ = 0; ///< audio thread: frames
    std::int32_t loopPass_ = 0; ///< audio thread: loop passes since the song started

    // Count-in and punch (ADR 0017), audio thread.
    Transport countClock_; ///< counts the count-in's ticks from 0; the song's transport stands still
    bool playPending_ = false; ///< a Play came with this block's commands
    bool countingIn_ = false;
    int countInBars_ = 0;
    bool countInClick_ = true;
    int countInBeats_ = 0; ///< the running count-in's length in beats
    int countInBeatsPerBar_ = 4;
    std::int64_t nextCountBeat_ = 0;
    int countInEnd_ = -1; ///< the frame in this block the song started at after a count-in; -1: none
    bool afterCountIn_ = false; ///< the song started from a count-in and has not moved since
    double recordFloor_ = 0.0; ///< where it started: MIDI heard before that goes there
    double punchStart_ = 0.0;
    double punchEnd_ = 0.0; ///< punch off when <= punchStart_
    std::atomic<int> countInBeatsLeft_{0};
    std::atomic<int> countInBarsLeft_{0};

    std::atomic<bool> playing_{false};
    std::atomic<bool> recordingStatus_{false};
    std::atomic<bool> previewPlaying_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> tempo_{120.0};
    std::atomic<double> sampleRateStatus_{48000.0};
    std::atomic<int> beatsPerBarStatus_{4};
    std::atomic<float> cpuLoad_{0.0f};
    std::array<std::atomic<float>, 2> outputPeak_{}; ///< left, right; see takeOutputPeak()
    std::atomic<int> latency_{0};
};

} // namespace vloom
