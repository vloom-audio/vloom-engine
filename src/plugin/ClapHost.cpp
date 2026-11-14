// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/ClapHost.hpp"

#include "plugin/EditorRunLoop.hpp"
#include "plugin/PluginFiles.hpp"
#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <clap/clap.h>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>

namespace vloom {

namespace {

using plugin_files::fail;
using Module = plugin_files::SharedLibrary;

/// The binary inside a .clap: the file itself, or on macOS the executable of
/// the bundle directory.
std::filesystem::path binaryOf(const std::filesystem::path& path)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return path / "Contents" / "MacOS" / path.stem();
    return path;
}

bool hasFeature(const clap_plugin_descriptor_t& descriptor, const char* feature)
{
    if (descriptor.features == nullptr)
        return false;
    for (const char* const* f = descriptor.features; *f != nullptr; ++f)
        if (std::strcmp(*f, feature) == 0)
            return true;
    return false;
}

std::string text(const char* s) { return s != nullptr ? std::string(s) : std::string(); }

/// One audio port's buffers, sized at activation.
struct PortBuffers {
    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
};

/// The window API editors are opened with on this platform (ADR 0009).
#if defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kWindowApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

/// A parameter changed by the plugin itself (in its editor), on its way
/// from the audio thread to the control thread.
struct ParameterEdit {
    std::uint32_t id = 0;
    double value = 0.0;
};

int toInt(std::uint32_t value) noexcept { return static_cast<int>(std::min<std::uint32_t>(value, INT_MAX)); }

union ClapEvent {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_param_value_t param;
};

/// A CLAP plugin as an AudioProcessor. The control thread is CLAP's main
/// thread; process() and reset() run on the audio thread.
class ClapProcessor final : public AudioProcessor {
public:
    static constexpr std::size_t kMaxEvents = 512;
    static constexpr std::uint32_t kMaxPorts = 16;
    static constexpr std::uint32_t kMaxPortChannels = 32;

    ClapProcessor(std::shared_ptr<ClapLibrary> library, PluginInfo info)
        : library_(std::move(library))
        , info_(std::move(info))
    {
        host_.clap_version = CLAP_VERSION;
        host_.host_data = this;
        host_.name = "Vloom";
        host_.vendor = "Vloom Audio";
        host_.url = "https://github.com/vloom-audio";
        host_.version = "0.1.0";
        host_.get_extension
            = [](const clap_host_t*, const char* id) -> const void* { return hostExtension(id); };
        host_.request_restart = [](const clap_host_t* host) {
            self(host)->restartRequested_.store(true, std::memory_order_relaxed);
        };
        host_.request_process = [](const clap_host_t*) { }; // the engine processes every block
        host_.request_callback = [](const clap_host_t* host) {
            self(host)->callbackRequested_.store(true, std::memory_order_relaxed);
        };
        inEvents_.ctx = this;
        inEvents_.size = [](const clap_input_events_t* list) {
            return static_cast<std::uint32_t>(static_cast<const ClapProcessor*>(list->ctx)->eventCount_);
        };
        inEvents_.get
            = [](const clap_input_events_t* list, std::uint32_t index) -> const clap_event_header_t* {
            const auto* p = static_cast<const ClapProcessor*>(list->ctx);
            return index < p->eventCount_ ? &p->events_[index].header : nullptr;
        };
        // Audio thread: parameter values the plugin reports (edits in its
        // editor) go to the control thread; everything else is dropped.
        outEvents_.ctx = this;
        outEvents_.try_push = [](const clap_output_events_t* list, const clap_event_header_t* e) {
            if (const auto edit = editOf(e))
                static_cast<ClapProcessor*>(list->ctx)->edits_.push(*edit);
            return true;
        };
        // Control thread (a flush while inactive): straight to the listener.
        mainOutEvents_.ctx = this;
        mainOutEvents_.try_push = [](const clap_output_events_t* list, const clap_event_header_t* e) {
            auto* p = static_cast<ClapProcessor*>(list->ctx);
            if (const auto edit = editOf(e); edit && p->listener_ != nullptr)
                p->listener_->parameterEdited(edit->id, edit->value);
            return true;
        };
        noEvents_.size = [](const clap_input_events_t*) -> std::uint32_t { return 0; };
        noEvents_.get
            = [](const clap_input_events_t*, std::uint32_t) -> const clap_event_header_t* { return nullptr; };
    }

    ~ClapProcessor() override
    {
        if (plugin_ != nullptr) {
            ClapProcessor::closeEditor();
            ClapProcessor::deactivate();
            plugin_->destroy(plugin_);
        }
    }

    bool create(const clap_plugin_factory_t& factory, const std::string& id, std::string* error)
    {
        plugin_ = factory.create_plugin(&factory, &host_, id.c_str());
        if (plugin_ == nullptr) {
            fail(error, "the plugin " + id + " could not be created");
            return false;
        }
        if (!plugin_->init(plugin_)) {
            plugin_->destroy(plugin_);
            plugin_ = nullptr;
            fail(error, "the plugin " + id + " failed to initialise");
            return false;
        }
        params_ = static_cast<const clap_plugin_params_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
        state_ = static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
        latency_
            = static_cast<const clap_plugin_latency_t*>(plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
        ports_ = static_cast<const clap_plugin_audio_ports_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
        gui_ = static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
        return true;
    }

    [[nodiscard]] PluginInfo info() const override { return info_; }

    [[nodiscard]] std::vector<ParameterInfo> parameters() const override
    {
        std::vector<ParameterInfo> list;
        if (params_ == nullptr)
            return list;
        const std::uint32_t count = params_->count(plugin_);
        for (std::uint32_t i = 0; i < count; ++i) {
            clap_param_info_t p{};
            if (!params_->get_info(plugin_, i, &p) || (p.flags & CLAP_PARAM_IS_HIDDEN) != 0)
                continue;
            p.name[CLAP_NAME_SIZE - 1] = '\0';
            ParameterInfo parameter;
            parameter.id = p.id;
            parameter.name = p.name;
            parameter.minValue = p.min_value;
            parameter.maxValue = p.max_value;
            parameter.defaultValue = p.default_value;
            parameter.stepped = (p.flags & CLAP_PARAM_IS_STEPPED) != 0;
            list.push_back(std::move(parameter));
        }
        return list;
    }

    bool activate(double sampleRate, int maxBlockFrames) override
    {
        deactivate();
        if (!configurePorts(maxBlockFrames))
            return false;
        if (!plugin_->activate(plugin_, sampleRate, 1, static_cast<std::uint32_t>(maxBlockFrames)))
            return false;
        active_ = true;
        processing_ = false;
        steadyTime_ = 0;
        restartRequested_.store(false, std::memory_order_relaxed);
        return true;
    }

    void deactivate() override
    {
        if (!active_)
            return;
        // CLAP wants stop_processing() on the audio thread; the host calls
        // this only once the audio thread no longer processes the plugin
        // (ADR 0006), so it is ordered after the last process().
        if (processing_)
            plugin_->stop_processing(plugin_);
        processing_ = false;
        plugin_->deactivate(plugin_);
        active_ = false;
    }

    [[nodiscard]] int latencyFrames() const override
    {
        return latency_ != nullptr && active_ ? static_cast<int>(latency_->get(plugin_)) : 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> saveState() const override
    {
        std::vector<std::uint8_t> data;
        if (state_ == nullptr)
            return data;
        clap_ostream_t stream{};
        stream.ctx = &data;
        stream.write = [](const clap_ostream_t* s, const void* buffer, std::uint64_t size) -> std::int64_t {
            auto* out = static_cast<std::vector<std::uint8_t>*>(s->ctx);
            const auto* bytes = static_cast<const std::uint8_t*>(buffer);
            out->insert(out->end(), bytes, bytes + size);
            return static_cast<std::int64_t>(size);
        };
        if (!state_->save(plugin_, &stream))
            data.clear();
        return data;
    }

    bool loadState(std::span<const std::uint8_t> data) override
    {
        if (state_ == nullptr)
            return false;
        struct Reader {
            std::span<const std::uint8_t> data;
            std::size_t at = 0;
        } reader{data};
        clap_istream_t stream{};
        stream.ctx = &reader;
        stream.read = [](const clap_istream_t* s, void* buffer, std::uint64_t size) -> std::int64_t {
            auto* r = static_cast<Reader*>(s->ctx);
            const std::size_t n
                = std::min<std::size_t>(static_cast<std::size_t>(size), r->data.size() - r->at);
            if (n > 0)
                std::memcpy(buffer, r->data.data() + r->at, n);
            r->at += n;
            return static_cast<std::int64_t>(n);
        };
        return state_->load(plugin_, &stream);
    }

    void idle() override
    {
        if (callbackRequested_.exchange(false, std::memory_order_relaxed))
            plugin_->on_main_thread(plugin_);
        runLoop_.run();
        // While active the plugin flushes in process(); inactive, it is done here.
        if (params_ != nullptr && !active_ && flushRequested_.exchange(false, std::memory_order_relaxed))
            params_->flush(plugin_, &noEvents_, &mainOutEvents_);
        ParameterEdit edit;
        while (edits_.pop(edit))
            if (listener_ != nullptr)
                listener_->parameterEdited(edit.id, edit.value);
    }

    [[nodiscard]] bool hasEditor() const override
    {
        return gui_ != nullptr && gui_->is_api_supported(plugin_, kWindowApi, false);
    }

    bool openEditor(void* parent, EditorListener* listener) override
    {
        closeEditor();
        if (parent == nullptr || !hasEditor() || !gui_->create(plugin_, kWindowApi, false))
            return false;
        clap_window_t window{};
        window.api = kWindowApi;
#if defined(_WIN32)
        window.win32 = parent;
#elif defined(__APPLE__)
        window.cocoa = parent;
#else
        window.x11 = static_cast<clap_xwnd>(reinterpret_cast<std::uintptr_t>(parent));
#endif
        listener_ = listener; // the plugin may ask for a size while it attaches
        editorOpen_ = true;
        if (!gui_->set_parent(plugin_, &window)) {
            gui_->destroy(plugin_);
            editorOpen_ = false;
            listener_ = nullptr;
            return false;
        }
        gui_->show(plugin_);
        return true;
    }

    void closeEditor() override
    {
        if (!editorOpen_)
            return;
        gui_->hide(plugin_);
        gui_->destroy(plugin_);
        editorOpen_ = false;
        listener_ = nullptr;
    }

    [[nodiscard]] bool isEditorOpen() const override { return editorOpen_; }

    [[nodiscard]] EditorSize editorSize() const override
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (!editorOpen_ || !gui_->get_size(plugin_, &width, &height))
            return {};
        return {toInt(width), toInt(height)};
    }

    [[nodiscard]] bool canResizeEditor() const override { return editorOpen_ && gui_->can_resize(plugin_); }

    EditorSize setEditorSize(EditorSize wanted) override
    {
        if (canResizeEditor()) {
            auto width = static_cast<std::uint32_t>(std::max(wanted.width, 0));
            auto height = static_cast<std::uint32_t>(std::max(wanted.height, 0));
            gui_->adjust_size(plugin_, &width, &height);
            gui_->set_size(plugin_, width, height);
        }
        return editorSize();
    }

    void process(const ProcessContext& context) noexcept override
    {
        if (!active_ || context.numFrames <= 0 || context.numChannels < 1)
            return;
        if (!processing_) {
            if (!plugin_->start_processing(plugin_))
                return; // not ready: the audio passes through
            processing_ = true;
        }
        const auto frames = static_cast<std::size_t>(context.numFrames);
        float* left = context.channels[0];
        float* right = context.numChannels > 1 ? context.channels[1] : context.channels[0];

        // Events: parameters (all at frame 0) then notes, already in frame order.
        eventCount_ = 0;
        for (const ParameterEvent& p : context.parameters) {
            if (eventCount_ == kMaxEvents)
                break;
            clap_event_param_value_t& e = events_[eventCount_++].param;
            e = {};
            e.header = header(sizeof(e), p.frame, CLAP_EVENT_PARAM_VALUE);
            e.param_id = p.id;
            e.note_id = -1;
            e.port_index = -1;
            e.channel = -1;
            e.key = -1;
            e.value = p.value;
        }
        for (const NoteEvent& n : context.notes) {
            if (eventCount_ == kMaxEvents)
                break;
            clap_event_note_t& e = events_[eventCount_++].note;
            e = {};
            e.header = header(
                sizeof(e), n.frame, n.type == NoteEvent::Type::On ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
            e.note_id = -1;
            e.port_index = 0;
            e.channel = 0;
            e.key = n.key;
            e.velocity = static_cast<double>(n.velocity);
        }

        // Audio: ours into the main input; every other buffer silent.
        for (std::size_t port = 0; port < inputs_.size(); ++port)
            for (std::size_t ch = 0; ch < inputs_[port].channels.size(); ++ch) {
                float* dst = inputs_[port].pointers[ch];
                if (port != 0)
                    std::fill_n(dst, frames, 0.0f);
                else if (inputs_[port].channels.size() == 1)
                    for (std::size_t i = 0; i < frames; ++i)
                        dst[i] = 0.5f * (left[i] + right[i]);
                else if (ch < 2)
                    std::copy_n(ch == 0 ? left : right, frames, dst);
                else
                    std::fill_n(dst, frames, 0.0f);
            }
        for (PortBuffers& port : outputs_)
            for (float* dst : port.pointers)
                std::fill_n(dst, frames, 0.0f);

        clap_event_transport_t song{};
        song.header = header(sizeof(song), 0, CLAP_EVENT_TRANSPORT);
        song.flags = static_cast<std::uint32_t>(CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
            | (context.transport.playing ? CLAP_TRANSPORT_IS_PLAYING : 0));
        song.song_pos_beats = static_cast<clap_beattime>(
            std::llround(context.transport.positionTicks / kPpq * static_cast<double>(CLAP_BEATTIME_FACTOR)));
        song.tempo = context.transport.tempo;

        clap_process_t call{};
        call.steady_time = steadyTime_;
        call.frames_count = static_cast<std::uint32_t>(frames);
        call.transport = &song;
        call.audio_inputs = inputBuffers_.data();
        call.audio_inputs_count = static_cast<std::uint32_t>(inputBuffers_.size());
        call.audio_outputs = outputBuffers_.data();
        call.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers_.size());
        call.in_events = &inEvents_;
        call.out_events = &outEvents_;
        const clap_process_status status = plugin_->process(plugin_, &call);
        steadyTime_ += static_cast<std::int64_t>(frames);
        if (status == CLAP_PROCESS_ERROR || outputs_.empty())
            return; // the audio stays as it came

        // The main output back. A plugin with no audio input (an
        // instrument) adds to what is there; an effect replaces it.
        const PortBuffers& out = outputs_[0];
        const float* outLeft = out.pointers[0];
        const float* outRight = out.pointers.size() > 1 ? out.pointers[1] : out.pointers[0];
        const bool add = inputs_.empty();
        for (std::size_t i = 0; i < frames; ++i) {
            left[i] = add ? left[i] + outLeft[i] : outLeft[i];
            if (right != left)
                right[i] = add ? right[i] + outRight[i] : outRight[i];
        }
    }

    void reset() noexcept override
    {
        if (active_)
            plugin_->reset(plugin_);
    }

private:
    static ClapProcessor* self(const clap_host_t* host)
    {
        return static_cast<ClapProcessor*>(host->host_data);
    }

    static std::optional<ParameterEdit> editOf(const clap_event_header_t* e) noexcept
    {
        if (e == nullptr || e->space_id != CLAP_CORE_EVENT_SPACE_ID || e->type != CLAP_EVENT_PARAM_VALUE
            || e->size < sizeof(clap_event_param_value_t))
            return std::nullopt;
        const auto* param = reinterpret_cast<const clap_event_param_value_t*>(e);
        if (!std::isfinite(param->value))
            return std::nullopt;
        return ParameterEdit{param->param_id, param->value};
    }

    /// The host extensions plugins may ask for (ADR 0009): editors, their
    /// timers and file descriptors, and parameter flushes.
    static const void* hostExtension(const char* id)
    {
        static const clap_host_gui_t gui = {
            [](const clap_host_t*) { }, // resize_hints_changed: asked again on each resize
            [](const clap_host_t* host, std::uint32_t width, std::uint32_t height) {
                ClapProcessor* p = self(host);
                if (!p->editorOpen_ || p->listener_ == nullptr)
                    return false;
                p->listener_->editorResizeRequested({toInt(width), toInt(height)});
                return true;
            },
            [](const clap_host_t* host) { return self(host)->editorOpen_; }, // request_show: it is shown
            [](const clap_host_t*) { return false; }, // request_hide: embedded editors stay
            [](const clap_host_t* host, bool wasDestroyed) {
                ClapProcessor* p = self(host);
                EditorListener* listener = p->listener_;
                if (wasDestroyed) {
                    p->editorOpen_ = false;
                    p->listener_ = nullptr;
                }
                if (listener != nullptr)
                    listener->editorClosed();
            },
        };
        static const clap_host_timer_support_t timers = {
            [](const clap_host_t* host, std::uint32_t periodMs, clap_id* timerId) {
                ClapProcessor* p = self(host);
                const std::uint32_t added = p->runLoop_.addTimer(
                    std::chrono::milliseconds{periodMs}, [p](std::uint32_t timer) { p->onTimer(timer); });
                if (added == 0 || timerId == nullptr)
                    return false;
                *timerId = added;
                return true;
            },
            [](const clap_host_t* host, clap_id timerId) {
                return self(host)->runLoop_.removeTimer(timerId);
            },
        };
#if !defined(_WIN32)
        static const clap_host_posix_fd_support_t fds = {
            [](const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) {
                ClapProcessor* p = self(host);
                return p->runLoop_.addFd(
                    fd, flags, [p, fd](std::uint32_t happened) { p->onFd(fd, happened); });
            },
            [](const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) {
                return self(host)->runLoop_.modifyFd(fd, flags);
            },
            [](const clap_host_t* host, int fd) { return self(host)->runLoop_.removeFd(fd); },
        };
#endif
        static const clap_host_params_t params = {
            [](const clap_host_t*, clap_param_rescan_flags) { }, // the host asks for parameters() each time
            [](const clap_host_t*, clap_id, clap_param_clear_flags) { },
            [](const clap_host_t* host) {
                self(host)->flushRequested_.store(true, std::memory_order_relaxed);
            },
        };
        if (std::strcmp(id, CLAP_EXT_GUI) == 0)
            return &gui;
        if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
            return &timers;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &params;
#if !defined(_WIN32)
        if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0)
            return &fds;
#endif
        return nullptr;
    }

    void onTimer(std::uint32_t timer)
    {
        if (timerSupport_ == nullptr)
            timerSupport_ = static_cast<const clap_plugin_timer_support_t*>(
                plugin_->get_extension(plugin_, CLAP_EXT_TIMER_SUPPORT));
        if (timerSupport_ != nullptr)
            timerSupport_->on_timer(plugin_, timer);
    }

    void onFd(int fd, std::uint32_t happened)
    {
        if (fdSupport_ == nullptr)
            fdSupport_ = static_cast<const clap_plugin_posix_fd_support_t*>(
                plugin_->get_extension(plugin_, CLAP_EXT_POSIX_FD_SUPPORT));
        if (fdSupport_ != nullptr)
            fdSupport_->on_fd(plugin_, fd, happened);
    }

    static clap_event_header_t header(std::size_t size, int frame, int type) noexcept
    {
        clap_event_header_t h{};
        h.size = static_cast<std::uint32_t>(size);
        h.time = static_cast<std::uint32_t>(std::max(frame, 0));
        h.space_id = CLAP_CORE_EVENT_SPACE_ID;
        h.type = static_cast<std::uint16_t>(type);
        return h;
    }

    /// Sizes a buffer for every channel of every audio port. False if the
    /// plugin asks for more than the host supports.
    bool configurePorts(int maxBlockFrames)
    {
        const auto size = [&](bool input, std::vector<PortBuffers>& ports,
                              std::vector<clap_audio_buffer_t>& buffers) {
            ports.clear();
            buffers.clear();
            const std::uint32_t count = ports_ != nullptr ? ports_->count(plugin_, input) : 0;
            if (count > kMaxPorts)
                return false;
            // The main port first: ours goes there.
            for (std::uint32_t i = 0; i < count; ++i) {
                clap_audio_port_info_t portInfo{};
                if (!ports_->get(plugin_, i, input, &portInfo) || portInfo.channel_count > kMaxPortChannels)
                    return false;
                PortBuffers port;
                port.channels.assign(std::max<std::uint32_t>(portInfo.channel_count, 1),
                    std::vector<float>(static_cast<std::size_t>(maxBlockFrames), 0.0f));
                for (auto& channel : port.channels)
                    port.pointers.push_back(channel.data());
                ports.push_back(std::move(port));
            }
            for (PortBuffers& port : ports) {
                clap_audio_buffer_t buffer{};
                buffer.data32 = port.pointers.data();
                buffer.channel_count = static_cast<std::uint32_t>(port.pointers.size());
                buffers.push_back(buffer);
            }
            return true;
        };
        return size(true, inputs_, inputBuffers_) && size(false, outputs_, outputBuffers_);
    }

    std::shared_ptr<ClapLibrary> library_; ///< kept loaded while the plugin lives
    PluginInfo info_;
    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* params_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_audio_ports_t* ports_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    const clap_plugin_timer_support_t* timerSupport_ = nullptr;
    const clap_plugin_posix_fd_support_t* fdSupport_ = nullptr;

    // Editor (control thread).
    bool editorOpen_ = false;
    EditorListener* listener_ = nullptr;
    plugin_editor::RunLoop runLoop_;
    std::atomic<bool> flushRequested_{false};
    SpscQueue<ParameterEdit, 256> edits_; ///< audio thread to control thread
    clap_output_events_t mainOutEvents_{};
    clap_input_events_t noEvents_{};

    bool active_ = false;
    bool processing_ = false; ///< audio thread, once active
    std::atomic<bool> restartRequested_{false};
    std::atomic<bool> callbackRequested_{false};

    std::vector<PortBuffers> inputs_, outputs_;
    std::vector<clap_audio_buffer_t> inputBuffers_, outputBuffers_;
    std::array<ClapEvent, kMaxEvents> events_{};
    std::size_t eventCount_ = 0;
    clap_input_events_t inEvents_{};
    clap_output_events_t outEvents_{};
    std::int64_t steadyTime_ = 0;
};

std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}

std::map<std::filesystem::path, std::weak_ptr<ClapLibrary>>& cache()
{
    static std::map<std::filesystem::path, std::weak_ptr<ClapLibrary>> c;
    return c;
}

} // namespace

struct ClapLibrary::Impl {
    std::filesystem::path path;
    Module module;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    std::vector<PluginInfo> plugins;
    bool initialised = false;

    ~Impl()
    {
        if (initialised)
            entry->deinit();
    }
};

ClapLibrary::ClapLibrary(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

ClapLibrary::~ClapLibrary() = default;

std::shared_ptr<ClapLibrary> ClapLibrary::open(const std::filesystem::path& path, std::string* error)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path key = ec ? path : canonical;
    const std::lock_guard lock(cacheMutex());
    if (auto it = cache().find(key); it != cache().end())
        if (auto library = it->second.lock())
            return library;

    auto impl = std::make_unique<Impl>();
    impl->path = key;
    if (!std::filesystem::exists(key, ec)) {
        fail(error, "no such file: " + key.string());
        return nullptr;
    }
    if (!impl->module.load(binaryOf(key), error))
        return nullptr;
    impl->entry = static_cast<const clap_plugin_entry_t*>(impl->module.symbol("clap_entry"));
    if (impl->entry == nullptr) {
        fail(error, key.string() + " is not a CLAP plugin (no clap_entry)");
        return nullptr;
    }
    if (!clap_version_is_compatible(impl->entry->clap_version)) {
        fail(error, key.string() + " needs a newer CLAP version");
        return nullptr;
    }
    if (!impl->entry->init(key.string().c_str())) {
        fail(error, key.string() + " failed to initialise");
        return nullptr;
    }
    impl->initialised = true;
    impl->factory
        = static_cast<const clap_plugin_factory_t*>(impl->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (impl->factory == nullptr) {
        fail(error, key.string() + " has no plugin factory");
        return nullptr;
    }
    const std::uint32_t count = impl->factory->get_plugin_count(impl->factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = impl->factory->get_plugin_descriptor(impl->factory, i);
        if (d == nullptr || d->id == nullptr || !clap_version_is_compatible(d->clap_version))
            continue;
        PluginInfo plugin;
        plugin.id = d->id;
        plugin.name = text(d->name);
        plugin.vendor = text(d->vendor);
        plugin.version = text(d->version);
        plugin.kind = hasFeature(*d, CLAP_PLUGIN_FEATURE_INSTRUMENT) ? PluginInfo::Kind::Instrument
                                                                     : PluginInfo::Kind::Effect;
        impl->plugins.push_back(std::move(plugin));
    }
    std::shared_ptr<ClapLibrary> library(new ClapLibrary(std::move(impl)));
    cache()[key] = library;
    return library;
}

const std::filesystem::path& ClapLibrary::path() const noexcept { return impl_->path; }

const std::vector<PluginInfo>& ClapLibrary::plugins() const noexcept { return impl_->plugins; }

std::unique_ptr<AudioProcessor> ClapLibrary::create(std::string_view id, std::string* error)
{
    const auto it = std::find_if(
        impl_->plugins.begin(), impl_->plugins.end(), [&](const PluginInfo& p) { return p.id == id; });
    if (it == impl_->plugins.end()) {
        fail(error, "no plugin " + std::string(id) + " in " + impl_->path.string());
        return nullptr;
    }
    auto processor = std::make_unique<ClapProcessor>(shared_from_this(), *it);
    if (!processor->create(*impl_->factory, it->id, error))
        return nullptr;
    return processor;
}

std::vector<std::filesystem::path> clapSearchPaths()
{
    std::vector<std::filesystem::path> paths = plugin_files::pathsFrom("CLAP_PATH");
    const auto environment = plugin_files::environment;
#if defined(_WIN32)
    if (const std::string common = environment("COMMONPROGRAMFILES"); !common.empty())
        paths.push_back(std::filesystem::path(common) / "CLAP");
    if (const std::string local = environment("LOCALAPPDATA"); !local.empty())
        paths.push_back(std::filesystem::path(local) / "Programs" / "Common" / "CLAP");
#elif defined(__APPLE__)
    paths.emplace_back("/Library/Audio/Plug-Ins/CLAP");
    if (const std::string home = environment("HOME"); !home.empty())
        paths.push_back(std::filesystem::path(home) / "Library" / "Audio" / "Plug-Ins" / "CLAP");
#else
    if (const std::string home = environment("HOME"); !home.empty())
        paths.push_back(std::filesystem::path(home) / ".clap");
    paths.emplace_back("/usr/lib/clap");
#endif
    return paths;
}

std::vector<std::filesystem::path> findClapFiles(const std::vector<std::filesystem::path>& directories)
{
    return plugin_files::findFiles(directories, ".clap");
}

} // namespace vloom
