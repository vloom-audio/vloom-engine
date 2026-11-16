// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// A .clap holding two tiny plugins the CLAP host tests load for real:
//
// - "vloom.test.level" (effect): output = input * Level (param 3, 0 .. 2),
//   stereo in and out, 8-byte state, 7 frames of latency. Its editor (no
//   real window: it only records the parent) is 300 x 200, resizable down to
//   100 x 100. As it attaches it asks for 400 x 300 and starts a timer; on
//   the timer's first tick it edits Level to 0.25 and asks for a flush, so
//   the edit comes out of params.flush() (inactive) or process() (active).
// - "vloom.test.keys" (instrument): no input; outputs 0.1 per held key from
//   the frame the note-on lands on, so a test can read note timing off the
//   audio. Its right channel is the tempo / 1000 while playing.
// - "vloom.test.pads" (instrument): like the keys, but with 40 stereo output
//   ports and a note port that takes MIDI only, so it counts keys from MIDI
//   events and ignores CLAP note events. It refuses to start without the
//   host's thread check, and a block that lacks a port or comes from a
//   thread the host does not call the audio thread. Its right channel is the
//   time signature's beats per bar / 100.
#include <algorithm>
#include <clap/clap.h>
#include <cstring>
#include <iterator>

namespace {

constexpr clap_id kLevel = 3;

#if defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kWindowApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

struct Plugin {
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    bool instrument = false;
    bool pads = false;
    bool guiCreated = false;
    std::uint32_t width = 300;
    std::uint32_t height = 200;
    clap_id timer = CLAP_INVALID_ID;
    double pendingEdit = -1.0; ///< an edit made in the editor, not yet reported
    double level = 1.0;
    int held[128]{};
    int count = 0;
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

const char* const kEffectFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kInstrumentFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};

const clap_plugin_descriptor_t kLevelDescriptor = {CLAP_VERSION_INIT, "vloom.test.level", "Test Level",
    "Vloom Tests", "", "", "", "1.0", "", kEffectFeatures};
const clap_plugin_descriptor_t kKeysDescriptor = {CLAP_VERSION_INIT, "vloom.test.keys", "Test Keys",
    "Vloom Tests", "", "", "", "1.0", "", kInstrumentFeatures};
const clap_plugin_descriptor_t kPadsDescriptor = {CLAP_VERSION_INIT, "vloom.test.pads", "Test Pads",
    "Vloom Tests", "", "", "", "1.0", "", kInstrumentFeatures};
constexpr std::uint32_t kPadsOutputs = 40;

// ---- params
std::uint32_t paramCount(const clap_plugin_t* p) { return self(p)->instrument ? 0 : 1; }
bool paramInfo(const clap_plugin_t*, std::uint32_t index, clap_param_info_t* info)
{
    if (index != 0)
        return false;
    *info = {};
    info->id = kLevel;
    std::strcpy(info->name, "Level");
    info->min_value = 0.0;
    info->max_value = 2.0;
    info->default_value = 1.0;
    return true;
}
bool paramValue(const clap_plugin_t* p, clap_id id, double* value)
{
    if (id != kLevel)
        return false;
    *value = self(p)->level;
    return true;
}
bool paramToText(const clap_plugin_t*, clap_id, double, char*, std::uint32_t) { return false; }
bool paramFromText(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
/// Reports an edit made in the editor, if there is one.
void reportEdit(Plugin* plugin, const clap_output_events_t* out)
{
    if (plugin->pendingEdit < 0.0 || out == nullptr)
        return;
    clap_event_param_value_t e{};
    e.header.size = sizeof(e);
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = kLevel;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = plugin->pendingEdit;
    if (out->try_push(out, &e.header))
        plugin->pendingEdit = -1.0;
}
void paramFlush(const clap_plugin_t* p, const clap_input_events_t*, const clap_output_events_t* out)
{
    reportEdit(self(p), out);
}
const clap_plugin_params_t kParams
    = {paramCount, paramInfo, paramValue, paramToText, paramFromText, paramFlush};

// ---- audio ports
std::uint32_t portCount(const clap_plugin_t* p, bool input)
{
    if (input)
        return self(p)->instrument ? 0 : 1;
    return self(p)->pads ? kPadsOutputs : 1;
}
bool portInfo(const clap_plugin_t* p, std::uint32_t index, bool input, clap_audio_port_info_t* info)
{
    if (index >= portCount(p, input))
        return false;
    *info = {};
    info->id = input ? 0 : 1 + index;
    std::strcpy(info->name, input ? "In" : "Out");
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portCount, portInfo};

// ---- note ports (the pads: MIDI only)
std::uint32_t notePortCount(const clap_plugin_t*, bool input) { return input ? 1 : 0; }
bool notePortInfo(const clap_plugin_t*, std::uint32_t index, bool input, clap_note_port_info_t* info)
{
    if (index != 0 || !input)
        return false;
    *info = {};
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::strcpy(info->name, "Notes");
    return true;
}
const clap_plugin_note_ports_t kNotePorts = {notePortCount, notePortInfo};

// ---- state, latency
bool save(const clap_plugin_t* p, const clap_ostream_t* stream)
{
    return stream->write(stream, &self(p)->level, sizeof(double))
        == static_cast<std::int64_t>(sizeof(double));
}
bool load(const clap_plugin_t* p, const clap_istream_t* stream)
{
    double level = 0.0;
    if (stream->read(stream, &level, sizeof(double)) != static_cast<std::int64_t>(sizeof(double)))
        return false;
    self(p)->level = level;
    return true;
}
const clap_plugin_state_t kState = {save, load};
std::uint32_t latency(const clap_plugin_t*) { return 7; }
const clap_plugin_latency_t kLatency = {latency};

// ---- editor
template <typename T> const T* hostExtension(Plugin* plugin, const char* id)
{
    return static_cast<const T*>(plugin->host->get_extension(plugin->host, id));
}
bool guiApiSupported(const clap_plugin_t*, const char* api, bool floating)
{
    return !floating && std::strcmp(api, kWindowApi) == 0;
}
bool guiPreferredApi(const clap_plugin_t*, const char** api, bool* floating)
{
    *api = kWindowApi;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating)
{
    if (!guiApiSupported(p, api, floating))
        return false;
    self(p)->guiCreated = true;
    return true;
}
void guiDestroy(const clap_plugin_t* p)
{
    Plugin* plugin = self(p);
    if (plugin->timer != CLAP_INVALID_ID)
        if (const auto* timers = hostExtension<clap_host_timer_support_t>(plugin, CLAP_EXT_TIMER_SUPPORT))
            timers->unregister_timer(plugin->host, plugin->timer);
    plugin->timer = CLAP_INVALID_ID;
    plugin->guiCreated = false;
}
bool guiSetScale(const clap_plugin_t*, double) { return true; }
bool guiGetSize(const clap_plugin_t* p, std::uint32_t* width, std::uint32_t* height)
{
    *width = self(p)->width;
    *height = self(p)->height;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return true; }
bool guiResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t*, std::uint32_t* width, std::uint32_t* height)
{
    *width = std::max<std::uint32_t>(*width, 100);
    *height = std::max<std::uint32_t>(*height, 100);
    return true;
}
bool guiSetSize(const clap_plugin_t* p, std::uint32_t width, std::uint32_t height)
{
    self(p)->width = width;
    self(p)->height = height;
    return true;
}
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window)
{
    Plugin* plugin = self(p);
    if (!plugin->guiCreated || window == nullptr || window->ptr == nullptr)
        return false;
    if (const auto* gui = hostExtension<clap_host_gui_t>(plugin, CLAP_EXT_GUI))
        gui->request_resize(plugin->host, 400, 300);
    if (const auto* timers = hostExtension<clap_host_timer_support_t>(plugin, CLAP_EXT_TIMER_SUPPORT))
        timers->register_timer(plugin->host, 0, &plugin->timer);
    return true;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) { }
bool guiShow(const clap_plugin_t*) { return true; }
bool guiHide(const clap_plugin_t*) { return true; }
const clap_plugin_gui_t kGui = {guiApiSupported, guiPreferredApi, guiCreate, guiDestroy, guiSetScale,
    guiGetSize, guiCanResize, guiResizeHints, guiAdjustSize, guiSetSize, guiSetParent, guiSetTransient,
    guiSuggestTitle, guiShow, guiHide};

void onTimer(const clap_plugin_t* p, clap_id)
{
    Plugin* plugin = self(p);
    if (plugin->pendingEdit >= 0.0 || plugin->level == 0.25)
        return; // edited once
    plugin->level = 0.25; // as if the user turned the editor's knob
    plugin->pendingEdit = 0.25;
    if (const auto* params = hostExtension<clap_host_params_t>(plugin, CLAP_EXT_PARAMS))
        params->request_flush(plugin->host);
}
const clap_plugin_timer_support_t kTimer = {onTimer};

// ---- plugin
bool init(const clap_plugin_t* p)
{
    Plugin* plugin = self(p);
    if (!plugin->pads)
        return true;
    const auto* threads = hostExtension<clap_host_thread_check_t>(plugin, CLAP_EXT_THREAD_CHECK);
    return threads != nullptr && threads->is_main_thread(plugin->host)
        && !threads->is_audio_thread(plugin->host);
}
void destroy(const clap_plugin_t* p) { delete self(p); }
bool activate(const clap_plugin_t*, double, std::uint32_t, std::uint32_t) { return true; }
void deactivate(const clap_plugin_t*) { }
bool startProcessing(const clap_plugin_t*) { return true; }
void stopProcessing(const clap_plugin_t*) { }
void reset(const clap_plugin_t* p)
{
    Plugin* plugin = self(p);
    std::fill(std::begin(plugin->held), std::end(plugin->held), 0);
    plugin->count = 0;
}

void handle(Plugin* plugin, const clap_event_header_t* e)
{
    if (e->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    if (e->type == CLAP_EVENT_PARAM_VALUE) {
        const auto* param = reinterpret_cast<const clap_event_param_value_t*>(e);
        if (param->param_id == kLevel)
            plugin->level = param->value;
    } else if (plugin->pads) {
        if (e->type != CLAP_EVENT_MIDI)
            return; // CLAP note events are not for it
        const auto* midi = reinterpret_cast<const clap_event_midi_t*>(e);
        const int status = midi->data[0] & 0xF0;
        if ((status != 0x90 && status != 0x80) || midi->data[1] > 127)
            return;
        int& held = plugin->held[midi->data[1]];
        const int was = held;
        held = status == 0x90 && midi->data[2] > 0 ? 1 : 0;
        plugin->count += held - was;
    } else if (e->type == CLAP_EVENT_NOTE_ON || e->type == CLAP_EVENT_NOTE_OFF) {
        const auto* note = reinterpret_cast<const clap_event_note_t*>(e);
        if (note->key < 0 || note->key > 127)
            return;
        int& held = plugin->held[note->key];
        const int was = held;
        held = e->type == CLAP_EVENT_NOTE_ON ? 1 : 0;
        plugin->count += held - was;
    }
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* process)
{
    Plugin* plugin = self(p);
    if (plugin->pads) {
        const auto* threads = hostExtension<clap_host_thread_check_t>(plugin, CLAP_EXT_THREAD_CHECK);
        if (process->audio_outputs_count != kPadsOutputs || threads == nullptr
            || !threads->is_audio_thread(plugin->host) || threads->is_main_thread(plugin->host))
            return CLAP_PROCESS_ERROR;
    }
    const std::uint32_t events = process->in_events->size(process->in_events);
    std::uint32_t next = 0;
    float* const* out = process->audio_outputs[0].data32;
    const bool playing
        = process->transport != nullptr && (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    for (std::uint32_t i = 0; i < process->frames_count; ++i) {
        for (; next < events; ++next) {
            const clap_event_header_t* e = process->in_events->get(process->in_events, next);
            if (e->time > i)
                break;
            handle(plugin, e);
        }
        if (plugin->instrument) {
            out[0][i] = 0.1f * static_cast<float>(plugin->count);
            if (plugin->pads) { // its second port carries 0.2
                process->audio_outputs[1].data32[0][i] = 0.2f;
                process->audio_outputs[1].data32[1][i] = 0.2f;
            }
            if (plugin->pads)
                out[1][i] = process->transport != nullptr
                        && (process->transport->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) != 0
                    ? static_cast<float>(process->transport->tsig_num) / 100.0f
                    : 0.0f;
            else
                out[1][i] = playing ? static_cast<float>(process->transport->tempo / 1000.0) : 0.0f;
        } else {
            float* const* in = process->audio_inputs[0].data32;
            out[0][i] = in[0][i] * static_cast<float>(plugin->level);
            out[1][i] = in[1][i] * static_cast<float>(plugin->level);
        }
    }
    reportEdit(plugin, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

const void* extension(const clap_plugin_t* p, const char* id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &kPorts;
    if (self(p)->pads && std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &kNotePorts;
    if (self(p)->instrument)
        return nullptr;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &kParams;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &kState;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &kLatency;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &kGui;
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
        return &kTimer;
    return nullptr;
}

void onMainThread(const clap_plugin_t*) { }

// ---- factory, entry
std::uint32_t pluginCount(const clap_plugin_factory_t*) { return 3; }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, std::uint32_t index)
{
    const clap_plugin_descriptor_t* const all[] = {&kLevelDescriptor, &kKeysDescriptor, &kPadsDescriptor};
    return index < 3 ? all[index] : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    const bool keys = std::strcmp(id, kKeysDescriptor.id) == 0;
    const bool pads = std::strcmp(id, kPadsDescriptor.id) == 0;
    if (!keys && !pads && std::strcmp(id, kLevelDescriptor.id) != 0)
        return nullptr;
    auto* plugin = new Plugin;
    plugin->host = host;
    plugin->instrument = keys || pads;
    plugin->pads = pads;
    plugin->clap = {pads ? &kPadsDescriptor
            : keys       ? &kKeysDescriptor
                         : &kLevelDescriptor,
        plugin, init, destroy, activate, deactivate, startProcessing, stopProcessing, reset, process,
        extension, onMainThread};
    return &plugin->clap;
}
const clap_plugin_factory_t kFactory = {pluginCount, descriptor, create};

bool entryInit(const char*) { return true; }
void entryDeinit() { }
const void* factory(const char* id)
{
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry
    = {CLAP_VERSION_INIT, entryInit, entryDeinit, factory};
