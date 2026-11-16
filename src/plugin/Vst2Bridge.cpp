// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/Vst2Bridge.hpp"

#include "plugin/PluginFiles.hpp"

#if defined(_WIN32)

#include "plugin/Vst2Abi.hpp"
#include "plugin/bridge/BridgeProtocol.hpp"
#include "vloom/engine/plugin/PluginInstance.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <system_error>

namespace vloom::vst2_bridge {

namespace {

using plugin_files::fail;

/// How long the control thread waits for an answer. Loading a large
/// instrument takes its time; everything else is quick.
constexpr DWORD kLoadTimeoutMs = 120'000;
constexpr DWORD kControlTimeoutMs = 20'000;
/// How long the audio thread waits for a block before it goes on without
/// it: twice the time the block lasts, within these bounds. Longer would
/// make every other sound of the engine late for one slow plugin.
constexpr DWORD kMinAudioWaitMs = 50;
constexpr DWORD kMaxAudioWaitMs = 200;

/// A kernel handle that closes itself.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept
        : handle_(handle)
    {
    }
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE handle = nullptr) noexcept
    {
        if (handle_ != nullptr)
            ::CloseHandle(handle_);
        handle_ = handle;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

/// A handle's number, as the bridge gets it on its command line. Handle
/// values fit 32 bits, also in a 64-bit process.
std::wstring number(HANDLE handle)
{
    return std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle)));
}

/// A VST 2 plugin in a bridge process. The control thread asks through the
/// control area of the shared memory; process() hands a block over and
/// waits for it: the one place the audio thread waits on another process,
/// which is what a bridge is.
class BridgedProcessor final : public AudioProcessor {
public:
    ~BridgedProcessor() override
    {
        if (alive()) {
            BridgedProcessor::closeEditor();
            BridgedProcessor::deactivate();
            // Quit is not answered by a bridge that is already gone.
            ask(bridge::Request::Quit, 0, 0, 0.0, 3'000);
        }
        if (process_ && ::WaitForSingleObject(process_.get(), 3'000) != WAIT_OBJECT_0)
            ::TerminateProcess(process_.get(), 1);
        if (shared_ != nullptr)
            ::UnmapViewOfFile(shared_);
        // job_ closes last: whatever is left of the bridge ends with it.
    }

    bool start(
        const std::filesystem::path& bridgeFile, const std::filesystem::path& plugin, std::string* error)
    {
        // Nothing here has a name another process could open: the bridge
        // inherits these six handles and no others.
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        mapping_.reset(::CreateFileMappingW(
            INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(bridge::Shared), nullptr));
        if (mapping_)
            shared_ = static_cast<bridge::Shared*>(
                ::MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Shared)));
        controlRequest_.reset(::CreateEventW(&inherit, FALSE, FALSE, nullptr));
        controlAnswer_.reset(::CreateEventW(&inherit, FALSE, FALSE, nullptr));
        audioRequest_.reset(::CreateEventW(&inherit, FALSE, FALSE, nullptr));
        audioAnswer_.reset(::CreateEventW(&inherit, FALSE, FALSE, nullptr));
        // This process, for the bridge to see it end; to wait on, no more.
        HANDLE self = nullptr;
        ::DuplicateHandle(
            ::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(), &self, SYNCHRONIZE, TRUE, 0);
        const Handle host(self);
        if (shared_ == nullptr || !controlRequest_ || !controlAnswer_ || !audioRequest_ || !audioAnswer_
            || !host) {
            fail(error, "the plugin bridge's shared memory could not be made");
            return false;
        }
        shared_->magic = bridge::kMagic;
        shared_->version = bridge::kVersion;

        std::array<HANDLE, 6> handles{mapping_.get(), controlRequest_.get(), controlAnswer_.get(),
            audioRequest_.get(), audioAnswer_.get(), host.get()};
        SIZE_T listSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
        std::vector<std::uint8_t> listMemory(listSize);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listMemory.data());
        if (!::InitializeProcThreadAttributeList(list, 1, 0, &listSize)
            || !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            fail(error, "the plugin bridge could not be given its handles");
            return false;
        }
        std::wstring command = L"\"" + bridgeFile.wstring() + L"\"";
        for (HANDLE handle : handles)
            command += L" " + number(handle);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        PROCESS_INFORMATION made{};
        // Its own folder as the working directory: not ours (could be Downloads).
        const std::wstring folder = bridgeFile.parent_path().wstring();
        const BOOL started = ::CreateProcessW(bridgeFile.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            folder.empty() ? nullptr : folder.c_str(), &startup.StartupInfo, &made);
        const DWORD startError = ::GetLastError();
        ::DeleteProcThreadAttributeList(list);
        // Ours stay ours: no later child process inherits them.
        for (HANDLE handle : handles)
            ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        if (!started) {
            fail(error,
                "the plugin bridge " + plugin_files::utf8(bridgeFile) + " could not be started (error "
                    + std::to_string(startError) + ")");
            return false;
        }
        ::CloseHandle(made.hThread);
        process_.reset(made.hProcess);
        // The bridge ends with this object, even if the host goes without
        // a word. (It also watches the host itself; this is the backstop.)
        job_.reset(::CreateJobObjectW(nullptr, nullptr));
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(
                    job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))
                || !::AssignProcessToJobObject(job_.get(), process_.get()))
                job_.reset(); // not allowed here: without it then
        }

        const std::string file = plugin_files::utf8(plugin);
        if (file.size() >= static_cast<std::size_t>(bridge::kTextSize)) {
            fail(error, "the plugin's path is too long for the bridge");
            return false;
        }
        std::memcpy(shared_->control.text, file.c_str(), file.size() + 1);
        if (!ask(bridge::Request::Load, 0, 0, 0.0, kLoadTimeoutMs)) {
            const std::string reason = alive() ? text(shared_->control.text) : std::string();
            fail(error, reason.empty() ? "the plugin did not load in the bridge" : reason);
            return false;
        }
        const bridge::PluginFacts& facts = shared_->plugin;
        parameters_ = std::clamp(facts.parameters, 0, 65536);
        inputs_ = std::clamp(facts.inputs, 0, 256);
        outputs_ = std::clamp(facts.outputs, 0, 256);
        flags_ = facts.flags;
        info_.name = text(facts.name);
        if (info_.name.empty())
            info_.name = plugin_files::utf8(plugin.stem());
        info_.vendor = text(facts.vendor);
        info_.version = std::to_string(facts.version);
        info_.id = plugin_files::hex8(static_cast<std::uint32_t>(facts.uniqueId));
        info_.kind = (flags_ & vst2::kIsSynth) != 0 ? PluginInfo::Kind::Instrument : PluginInfo::Kind::Effect;
        // The values it starts with are the nearest thing to defaults.
        defaults_ = parameterValues();
        defaults_.resize(static_cast<std::size_t>(parameters_), 0.0f);
        return true;
    }

    [[nodiscard]] PluginInfo info() const override { return info_; }

    [[nodiscard]] std::vector<ParameterInfo> parameters() const override
    {
        std::vector<ParameterInfo> list;
        list.reserve(static_cast<std::size_t>(parameters_));
        for (int i = 0; i < parameters_; ++i) {
            ParameterInfo parameter;
            parameter.id = static_cast<std::uint32_t>(i);
            if (ask(bridge::Request::ParameterName, i))
                parameter.name = text(shared_->control.text);
            if (parameter.name.empty())
                parameter.name = "Parameter " + std::to_string(i + 1);
            if (ask(bridge::Request::ParameterLabel, i))
                parameter.unit = text(shared_->control.text);
            parameter.minValue = 0.0;
            parameter.maxValue = 1.0;
            const float value = defaults_[static_cast<std::size_t>(i)];
            parameter.defaultValue
                = std::isfinite(value) ? std::clamp(static_cast<double>(value), 0.0, 1.0) : 0.0;
            list.push_back(std::move(parameter));
        }
        return list;
    }

    [[nodiscard]] std::optional<double> parameterValue(std::uint32_t id) const override
    {
        if (id >= static_cast<std::uint32_t>(parameters_)
            || !ask(bridge::Request::GetParameter, static_cast<int>(id)))
            return std::nullopt;
        const double value = shared_->control.number;
        return std::isfinite(value) ? std::optional<double>(std::clamp(value, 0.0, 1.0)) : std::nullopt;
    }

    [[nodiscard]] bool hasFailed() const override { return !alive(); }

    bool activate(double sampleRate, int maxBlockFrames) override
    {
        deactivate();
        if (maxBlockFrames <= 0 || maxBlockFrames > bridge::kMaxFrames
            || !ask(bridge::Request::Activate, maxBlockFrames, 0, sampleRate))
            return false;
        sampleRate_ = sampleRate;
        blockFrames_ = maxBlockFrames;
        held_.fill(false);
        resetPending_.store(false, std::memory_order_relaxed);
        awaiting_ = false;
        active_ = true;
        return true;
    }

    void deactivate() override
    {
        if (!active_)
            return;
        active_ = false;
        // A block the bridge is still busy with ends first: the plugin must
        // not be deactivated under it.
        if (awaiting_ && alive()) {
            const HANDLE handles[2] = {audioAnswer_.get(), process_.get()};
            ::WaitForMultipleObjects(2, handles, FALSE, kControlTimeoutMs);
        }
        awaiting_ = false;
        ask(bridge::Request::Deactivate);
    }

    [[nodiscard]] int latencyFrames() const override
    {
        return active_ && alive() ? std::clamp(shared_->plugin.latency, 0, 1 << 30) : 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> saveState() const override
    {
        // The same form as a plugin loaded in this process (Vst2Host.cpp).
        // Nothing at all from a bridge that is gone: an empty state must
        // not be taken for the plugin's and saved over the last good one.
        std::vector<std::uint8_t> out{'V', 'L', 'V', '2'};
        if ((flags_ & vst2::kProgramChunks) != 0) {
            if (!ask(bridge::Request::GetChunk))
                return {};
            const std::vector<std::uint8_t> chunk = takeBlob();
            if (chunk.empty())
                return {};
            out.push_back('C');
            out.insert(out.end(), chunk.begin(), chunk.end());
            return out;
        }
        const std::vector<float> values = parameterValues();
        if (!alive() || values.size() != static_cast<std::size_t>(parameters_))
            return {};
        out.push_back('P');
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
        out.insert(out.end(), bytes, bytes + values.size() * sizeof(float));
        return out;
    }

    bool loadState(std::span<const std::uint8_t> data) override
    {
        if (data.size() < 5 || std::memcmp(data.data(), "VLV2", 4) != 0)
            return false;
        const std::span<const std::uint8_t> body = data.subspan(5);
        if (data[4] == 'C') {
            if ((flags_ & vst2::kProgramChunks) == 0 || body.empty()
                || body.size() > static_cast<std::size_t>(bridge::kMaxBlobBytes) || !alive())
                return false;
            // A block of memory for this one transfer, put into the bridge
            // by handle; the bridge closes its copy.
            const auto size = static_cast<std::uint64_t>(body.size());
            const Handle blob(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFFu), nullptr));
            void* view = blob ? ::MapViewOfFile(blob.get(), FILE_MAP_WRITE, 0, 0, body.size()) : nullptr;
            if (view == nullptr)
                return false;
            std::memcpy(view, body.data(), body.size());
            ::UnmapViewOfFile(view);
            HANDLE theirs = nullptr;
            if (!::DuplicateHandle(
                    ::GetCurrentProcess(), blob.get(), process_.get(), &theirs, FILE_MAP_READ, FALSE, 0))
                return false;
            return ask(bridge::Request::SetChunk, 0, static_cast<std::int64_t>(body.size()), 0.0,
                kControlTimeoutMs, static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(theirs)));
        }
        if (data[4] != 'P' || body.size() % sizeof(float) != 0)
            return false;
        const int count = std::min(parameters_, static_cast<int>(body.size() / sizeof(float)));
        for (int i = 0; i < count; ++i) {
            float value = 0.0f;
            std::memcpy(&value, body.data() + static_cast<std::size_t>(i) * sizeof(float), sizeof(float));
            if (std::isfinite(value))
                ask(bridge::Request::SetParameter, i, 0, static_cast<double>(value));
        }
        return alive();
    }

    void process(const ProcessContext& context) noexcept override
    {
        if (!active_ || dead_.load(std::memory_order_relaxed) || context.numFrames <= 0
            || context.numFrames > blockFrames_ || context.numChannels < 1)
            return;
        // A block the bridge did not finish in time is still its: nothing
        // new until it has answered. Meanwhile the audio passes through.
        if (awaiting_) {
            const HANDLE handles[2] = {audioAnswer_.get(), process_.get()};
            const DWORD woke = ::WaitForMultipleObjects(2, handles, FALSE, 0);
            if (woke == WAIT_TIMEOUT)
                return;
            if (woke != WAIT_OBJECT_0) {
                dead_.store(true, std::memory_order_relaxed);
                return;
            }
            awaiting_ = false; // that block's sound is stale: dropped
        }
        const auto frames = static_cast<std::size_t>(context.numFrames);
        float* left = context.channels[0];
        float* right = context.numChannels > 1 ? context.channels[1] : context.channels[0];
        bridge::Audio& audio = shared_->audio;
        audio.frames = context.numFrames;

        std::int32_t changes = 0;
        for (const ParameterEvent& p : context.parameters)
            if (changes < bridge::kMaxParameterChanges && p.id < static_cast<std::uint32_t>(parameters_)
                && std::isfinite(p.value))
                audio.changes[changes++]
                    = {static_cast<std::int32_t>(p.id), static_cast<float>(std::clamp(p.value, 0.0, 1.0))};
        audio.parameterChanges = changes;

        // Notes as MIDI. After a reset, every held key ends first.
        std::int32_t count = 0;
        const auto note = [&](int frame, bool on, int key, float velocity) {
            if (count == bridge::kMaxEvents)
                return;
            bridge::MidiMessage& m = audio.midi[count++];
            m.frame = std::clamp(frame, 0, context.numFrames - 1);
            m.data[0] = on ? 0x90 : 0x80;
            m.data[1] = static_cast<std::uint8_t>(key);
            // A note-on at velocity 0 would mean note-off.
            m.data[2] = on
                ? static_cast<std::uint8_t>(std::clamp<long>(std::lround(velocity * 127.0f), 1, 127))
                : std::uint8_t{0};
            m.data[3] = 0;
            held_[static_cast<std::size_t>(key)] = on;
        };
        // Controls as MIDI, before the notes (they come at the block's start).
        for (const ControlEvent& control : context.controls) {
            // Room is kept for the note-offs after a reset and for the block's notes.
            if (count + 128 + static_cast<std::int32_t>(PluginInstance::kMaxNotes) >= bridge::kMaxEvents
                || !std::isfinite(control.value))
                break;
            bridge::MidiMessage& m = audio.midi[count++];
            m.frame = std::clamp(control.frame, 0, context.numFrames - 1);
            const auto seven = [](float value) {
                return static_cast<std::uint8_t>(std::clamp<long>(std::lround(value * 127.0f), 0, 127));
            };
            if (control.type == ControlEvent::Type::PitchBend) {
                const long bend
                    = std::clamp<long>(std::lround((control.value + 1.0f) * 0.5f * 16383.0f), 0, 16383);
                m.data[0] = 0xE0;
                m.data[1] = static_cast<std::uint8_t>(bend & 0x7F);
                m.data[2] = static_cast<std::uint8_t>(bend >> 7);
            } else if (control.type == ControlEvent::Type::Controller) {
                m.data[0] = 0xB0;
                m.data[1] = static_cast<std::uint8_t>(std::clamp<int>(control.number, 0, 127));
                m.data[2] = seven(control.value);
            } else {
                m.data[0] = 0xD0;
                m.data[1] = seven(control.value);
                m.data[2] = 0;
            }
            m.data[3] = 0;
        }
        if (resetPending_.exchange(false, std::memory_order_relaxed))
            for (int key = 0; key < 128; ++key)
                if (held_[static_cast<std::size_t>(key)])
                    note(0, false, key, 0.0f);
        for (const NoteEvent& n : context.notes)
            if (n.key >= 0 && n.key < 128)
                note(n.frame, n.type == NoteEvent::Type::On, n.key, std::clamp(n.velocity, 0.0f, 1.0f));
        audio.events = count;

        const TransportInfo& song = context.transport;
        bridge::SongTime& time = audio.time;
        time = {};
        time.sampleRate = sampleRate_;
        time.tempo = song.tempo;
        time.ppqPos = song.positionTicks / kPpq;
        time.samplePos = song.tempo > 0.0 ? time.ppqPos * 60.0 / song.tempo * sampleRate_ : 0.0;
        time.flags = vst2::kPpqPosValid | vst2::kTempoValid | (song.playing ? vst2::kTransportPlaying : 0);
        if (song.beatsPerBar > 0 && song.beatUnit > 0) {
            const double barBeats = song.beatsPerBar * 4.0 / song.beatUnit;
            time.timeSigNumerator = song.beatsPerBar;
            time.timeSigDenominator = song.beatUnit;
            time.barStartPos = std::floor(time.ppqPos / barBeats) * barBeats;
            time.flags |= vst2::kTimeSigValid | vst2::kBarsValid;
        }
        if (song.loopEndTicks > song.loopStartTicks) {
            time.cycleStartPos = song.loopStartTicks / kPpq;
            time.cycleEndPos = song.loopEndTicks / kPpq;
            time.flags |= vst2::kCyclePosValid | vst2::kTransportCycleActive;
        }

        if (inputs_ > 0) {
            std::copy_n(left, frames, audio.in[0]);
            std::copy_n(right, frames, audio.in[1]);
        }
        ::SetEvent(audioRequest_.get());
        const HANDLE handles[2] = {audioAnswer_.get(), process_.get()};
        const double blockMs = 1000.0 * context.numFrames / sampleRate_;
        const DWORD wait = std::clamp(static_cast<DWORD>(2.0 * blockMs), kMinAudioWaitMs, kMaxAudioWaitMs);
        const DWORD woke = ::WaitForMultipleObjects(2, handles, FALSE, wait);
        if (woke == WAIT_TIMEOUT) {
            awaiting_ = true; // slow, not gone (a sampler loading): back when it answers
            return;
        }
        if (woke != WAIT_OBJECT_0) {
            dead_.store(true, std::memory_order_relaxed); // the bridge ended: the audio stays as it came
            return;
        }
        if (outputs_ == 0)
            return;
        // A plugin with no audio input (an instrument) adds to what is
        // there; an effect replaces it.
        const bool add = inputs_ == 0;
        for (std::size_t i = 0; i < frames; ++i) {
            left[i] = add ? left[i] + audio.out[0][i] : audio.out[0][i];
            if (right != left)
                right[i] = add ? right[i] + audio.out[1][i] : audio.out[1][i];
        }
    }

    void reset() noexcept override
    {
        // VST 2 has no reset: end every held note in the next block.
        resetPending_.store(true, std::memory_order_relaxed);
    }

    void idle() override
    {
        // An editor whose bridge is gone is gone too: the host is told.
        if (editorOpen_ && !alive()) {
            EditorListener* listener = listener_;
            editorOpen_ = false;
            listener_ = nullptr;
            if (listener != nullptr)
                listener->editorClosed();
            return;
        }
        // What the plugin told the bridge since the last time. The bridge
        // runs the editor's own idle time itself.
        bridge::Notices& notices = shared_->notices;
        const std::int32_t written = notices.written;
        std::int32_t read = notices.read;
        if (static_cast<std::uint32_t>(written - read) > static_cast<std::uint32_t>(bridge::kMaxNotices))
            read = written; // not a state the bridge leaves it in: skip it all
        for (; read != written; ++read) {
            const bridge::Notice notice
                = notices.items[static_cast<std::uint32_t>(read) % bridge::kMaxNotices];
            if (listener_ == nullptr)
                continue;
            if (notice.kind == 1 && notice.index >= 0 && notice.index < parameters_
                && std::isfinite(notice.value))
                listener_->parameterEdited(static_cast<std::uint32_t>(notice.index),
                    std::clamp(static_cast<double>(notice.value), 0.0, 1.0));
            else if (notice.kind == 2 && editorOpen_)
                listener_->editorResizeRequested(
                    {std::clamp(notice.index, 0, 1 << 20), std::clamp(notice.size, 0, 1 << 20)});
        }
        ::InterlockedExchange(reinterpret_cast<volatile LONG*>(&notices.read), read);
    }

    [[nodiscard]] bool hasEditor() const override { return (flags_ & vst2::kHasEditor) != 0 && alive(); }

    bool openEditor(void* parent, EditorListener* listener) override
    {
        closeEditor();
        if (parent == nullptr || !hasEditor())
            return false;
        listener_ = listener;
        if (!ask(bridge::Request::EditorOpen, 0,
                static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(parent)))) {
            listener_ = nullptr;
            return false;
        }
        editorOpen_ = true;
        idle(); // what it asked for while it opened
        return true;
    }

    void closeEditor() override
    {
        if (!editorOpen_)
            return;
        ask(bridge::Request::EditorClose);
        editorOpen_ = false;
        listener_ = nullptr;
    }

    [[nodiscard]] bool isEditorOpen() const override { return editorOpen_; }

    [[nodiscard]] EditorSize editorSize() const override
    {
        if (!editorOpen_ || !ask(bridge::Request::EditorSize))
            return {};
        return {std::clamp(shared_->control.index, 0, 1 << 20),
            static_cast<int>(std::clamp<std::int64_t>(shared_->control.value, 0, 1 << 20))};
    }

private:
    [[nodiscard]] bool alive() const noexcept
    {
        return process_ && !dead_.load(std::memory_order_relaxed)
            && ::WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
    }

    /// Text the bridge wrote: up to its buffer's end.
    template <std::size_t N> [[nodiscard]] static std::string text(const char (&buffer)[N])
    {
        return plugin_files::printable(std::string(buffer, std::find(buffer, buffer + N, char{})));
    }

    /// One request, answered; true if the bridge said yes. Control thread.
    ///
    /// While it waits it lets messages other windows SEND to this thread's
    /// windows through. The plugin's editor is a child of a window of this
    /// thread: creating, moving and closing it sends that window messages
    /// and waits for the answer, which a plain wait here would never give.
    bool ask(bridge::Request request, std::int32_t index = 0, std::int64_t value = 0, double number = 0.0,
        DWORD timeoutMs = kControlTimeoutMs, std::int32_t blob = 0) const
    {
        if (!alive())
            return false;
        bridge::Control& control = shared_->control;
        control.request = static_cast<std::int32_t>(request);
        control.index = index;
        control.value = value;
        control.number = number;
        control.result = 0;
        control.blob = blob;
        ::SetEvent(controlRequest_.get());
        const HANDLE handles[2] = {controlAnswer_.get(), process_.get()};
        const ULONGLONG start = ::GetTickCount64();
        for (;;) {
            const ULONGLONG waited = ::GetTickCount64() - start;
            const DWORD woke = waited >= timeoutMs
                ? WAIT_TIMEOUT
                : ::MsgWaitForMultipleObjects(
                      2, handles, FALSE, timeoutMs - static_cast<DWORD>(waited), QS_SENDMESSAGE);
            if (woke == WAIT_OBJECT_0)
                return control.result != 0;
            if (woke != WAIT_OBJECT_0 + 2) {
                dead_.store(true, std::memory_order_relaxed); // gone, or stuck for good
                return false;
            }
            MSG message;
            ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); // runs the sent ones
        }
    }

    /// The blob of the last answer, copied out of the bridge's memory.
    [[nodiscard]] std::vector<std::uint8_t> takeBlob() const
    {
        const bridge::Control& control = shared_->control;
        const std::int64_t size = control.value;
        const std::int32_t theirs = control.blob;
        if (theirs == 0 || size <= 0 || size > bridge::kMaxBlobBytes)
            return {};
        HANDLE copied = nullptr;
        if (!::DuplicateHandle(process_.get(),
                reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(theirs))),
                ::GetCurrentProcess(), &copied, FILE_MAP_READ, FALSE, 0))
            return {};
        const Handle blob(copied);
        void* view = ::MapViewOfFile(blob.get(), FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(size));
        if (view == nullptr)
            return {}; // not a block of memory, or a smaller one than it said
        std::vector<std::uint8_t> out;
        // All of it must be there to read: the size is only what the bridge said.
        MEMORY_BASIC_INFORMATION region{};
        if (::VirtualQuery(view, &region, sizeof(region)) != 0
            && region.RegionSize >= static_cast<SIZE_T>(size) && region.State == MEM_COMMIT) {
            try {
                const auto* bytes = static_cast<const std::uint8_t*>(view);
                out.assign(bytes, bytes + size);
            } catch (const std::bad_alloc&) {
                out.clear();
            }
        }
        ::UnmapViewOfFile(view);
        return out;
    }

    [[nodiscard]] std::vector<float> parameterValues() const
    {
        std::vector<float> values;
        if (parameters_ == 0 || !ask(bridge::Request::GetParameters))
            return values;
        const std::vector<std::uint8_t> bytes = takeBlob();
        values.resize(std::min(bytes.size() / sizeof(float), static_cast<std::size_t>(parameters_)));
        if (!values.empty())
            std::memcpy(values.data(), bytes.data(), values.size() * sizeof(float));
        return values;
    }

    Handle job_; ///< first: closed last, it ends whatever is left of the bridge
    Handle mapping_, controlRequest_, controlAnswer_, audioRequest_, audioAnswer_, process_;
    bridge::Shared* shared_ = nullptr;
    mutable std::atomic<bool> dead_{false}; ///< the bridge ended, or no longer answers the control thread

    PluginInfo info_;
    int parameters_ = 0;
    int inputs_ = 0;
    int outputs_ = 0;
    std::int32_t flags_ = 0;
    std::vector<float> defaults_;

    bool editorOpen_ = false;
    EditorListener* listener_ = nullptr;

    bool active_ = false;
    bool awaiting_ = false; ///< audio thread: a block is still with the bridge
    double sampleRate_ = 48000.0;
    int blockFrames_ = 0;
    std::atomic<bool> resetPending_{false};
    std::array<bool, 128> held_{}; ///< audio thread: keys it was told are down
};

} // namespace

std::unique_ptr<AudioProcessor> create(
    const std::filesystem::path& bridge, const std::filesystem::path& plugin, std::string* error)
{
    auto processor = std::make_unique<BridgedProcessor>();
    if (!processor->start(bridge, plugin, error))
        return nullptr;
    return processor;
}

bool needs32BitBridge(const std::filesystem::path& file)
{
#if defined(_WIN64)
    return plugin_files::readModule(file, {}).machine == 0x014C; // x86
#else
    (void)file;
    return false;
#endif
}

std::filesystem::path find32BitBridge()
{
    std::error_code ec;
    if (const std::vector<std::filesystem::path> named = plugin_files::pathsFrom("VLOOM_PLUGIN_BRIDGE32");
        !named.empty() && std::filesystem::is_regular_file(named.front(), ec))
        return named.front();
    std::vector<wchar_t> program(32768);
    const DWORD length = ::GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
    if (length == 0 || length >= program.size())
        return {};
    const std::filesystem::path beside
        = std::filesystem::path(program.data()).parent_path() / "vloom-plugin-bridge32.exe";
    return std::filesystem::is_regular_file(beside, ec) ? beside : std::filesystem::path();
}

} // namespace vloom::vst2_bridge

#else // not Windows

namespace vloom::vst2_bridge {

std::unique_ptr<AudioProcessor> create(
    const std::filesystem::path&, const std::filesystem::path&, std::string* error)
{
    plugin_files::fail(error, "the plugin bridge is only there on Windows");
    return nullptr;
}

bool needs32BitBridge(const std::filesystem::path&) { return false; }

std::filesystem::path find32BitBridge() { return {}; }

} // namespace vloom::vst2_bridge

#endif
