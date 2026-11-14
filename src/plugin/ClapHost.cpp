// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/ClapHost.hpp"

#include "plugin/PluginFiles.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <clap/clap.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
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
        host_.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
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
        outEvents_.ctx = this;
        outEvents_.try_push = [](const clap_output_events_t*, const clap_event_header_t*) { return true; };
    }

    ~ClapProcessor() override
    {
        if (plugin_ != nullptr) {
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
