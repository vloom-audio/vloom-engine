// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// A .clap holding two tiny plugins the CLAP host tests load for real:
//
// - "vloom.test.level" (effect): output = input * Level (param 3, 0 .. 2),
//   stereo in and out, 8-byte state, 7 frames of latency.
// - "vloom.test.keys" (instrument): no input; outputs 0.1 per held key from
//   the frame the note-on lands on, so a test can read note timing off the
//   audio. Its right channel is the tempo / 1000 while playing.
#include <algorithm>
#include <clap/clap.h>
#include <cstring>

namespace {

constexpr clap_id kLevel = 3;

struct Plugin {
    clap_plugin_t clap{};
    bool instrument = false;
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
void paramFlush(const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) { }
const clap_plugin_params_t kParams
    = {paramCount, paramInfo, paramValue, paramToText, paramFromText, paramFlush};

// ---- audio ports
std::uint32_t portCount(const clap_plugin_t* p, bool input) { return input && self(p)->instrument ? 0 : 1; }
bool portInfo(const clap_plugin_t*, std::uint32_t index, bool input, clap_audio_port_info_t* info)
{
    if (index != 0)
        return false;
    *info = {};
    info->id = input ? 0 : 1;
    std::strcpy(info->name, input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portCount, portInfo};

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

// ---- plugin
bool init(const clap_plugin_t*) { return true; }
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
            out[1][i] = playing ? static_cast<float>(process->transport->tempo / 1000.0) : 0.0f;
        } else {
            float* const* in = process->audio_inputs[0].data32;
            out[0][i] = in[0][i] * static_cast<float>(plugin->level);
            out[1][i] = in[1][i] * static_cast<float>(plugin->level);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* extension(const clap_plugin_t* p, const char* id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &kPorts;
    if (self(p)->instrument)
        return nullptr;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &kParams;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &kState;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &kLatency;
    return nullptr;
}

void onMainThread(const clap_plugin_t*) { }

// ---- factory, entry
std::uint32_t pluginCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, std::uint32_t index)
{
    return index == 0 ? &kLevelDescriptor : index == 1 ? &kKeysDescriptor : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t*, const char* id)
{
    const bool keys = std::strcmp(id, kKeysDescriptor.id) == 0;
    if (!keys && std::strcmp(id, kLevelDescriptor.id) != 0)
        return nullptr;
    auto* plugin = new Plugin;
    plugin->instrument = keys;
    plugin->clap = {keys ? &kKeysDescriptor : &kLevelDescriptor, plugin, init, destroy, activate, deactivate,
        startProcessing, stopProcessing, reset, process, extension, onMainThread};
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
