// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// A real VST 3 module for the host tests (plugin/Vst3HostTests.cpp), written
// against the plain interfaces, like the test .clap:
//   "Level"  effect, one object as component and controller: multiplies by
//            2 x parameter 3 (normalized, default 0.5 = unity); state = that
//            value; latency 7 frames.
//   "Keys"   instrument, a component with a separate controller connected
//            to it: left = 0.1 per held key, right = tempo / 1000 while the
//            host plays. No audio input.
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

bool same(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

const TUID kLevelId = INLINE_UID(0x564C4F4F, 0x4D544553, 0x544C4556, 0x454C0001);
const TUID kKeysId = INLINE_UID(0x564C4F4F, 0x4D544553, 0x544B4559, 0x53000002);
const TUID kKeysControllerId = INLINE_UID(0x564C4F4F, 0x4D544553, 0x544B4559, 0x53430003);

void copyName(String128 out, const char* name)
{
    std::size_t i = 0;
    for (; name[i] != 0 && i < 127; ++i)
        out[i] = static_cast<TChar>(name[i]);
    out[i] = 0;
}

/// FUnknown with a heap reference count, for one concrete class.
#define TEST_REFCOUNT                                                                                        \
    uint32 PLUGIN_API addRef() override { return ++refs_; }                                                  \
    uint32 PLUGIN_API release() override                                                                     \
    {                                                                                                        \
        const uint32 left = --refs_;                                                                         \
        if (left == 0)                                                                                       \
            delete this;                                                                                     \
        return left;                                                                                         \
    }                                                                                                        \
    std::atomic<uint32> refs_{1};

/// What both test processors share: a component that is also an audio
/// processor, with buses as given.
class ProcessorBase : public IComponent, public IAudioProcessor {
public:
    ProcessorBase(int audioInputs, int eventInputs)
        : audioInputs_(audioInputs)
        , eventInputs_(eventInputs)
    {
    }
    virtual ~ProcessorBase() = default;

    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override
    {
        if (type == kAudio)
            return dir == kInput ? audioInputs_ : 1;
        return dir == kInput ? eventInputs_ : 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override
    {
        if (index != 0 || index >= getBusCount(type, dir))
            return kInvalidArgument;
        bus = {};
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kAudio ? 2 : 16;
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        copyName(bus.name, type == kAudio ? "Main" : "Notes");
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kResultFalse; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }

    tresult PLUGIN_API setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) override
    {
        if (numIns != audioInputs_ || numOuts != 1)
            return kResultFalse;
        for (int32 i = 0; i < numIns; ++i)
            if (inputs[i] != SpeakerArr::kStereo)
                return kResultFalse;
        return outputs[0] == SpeakerArr::kStereo ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override
    {
        if (index != 0)
            return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override
    {
        return size == kSample32 ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

protected:
    int32 audioInputs_;
    int32 eventInputs_;
};

class Level final : public ProcessorBase, public IEditController {
public:
    Level()
        : ProcessorBase(1, 0)
    {
    }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, IComponent::iid) || same(wanted, IPluginBase::iid))
            *obj = static_cast<IComponent*>(this);
        else if (same(wanted, IAudioProcessor::iid))
            *obj = static_cast<IAudioProcessor*>(this);
        else if (same(wanted, IEditController::iid))
            *obj = static_cast<IEditController*>(this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    TEST_REFCOUNT

    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; } // it is its own

    tresult PLUGIN_API setState(IBStream* state) override
    {
        double value = 0.0;
        int32 read = 0;
        if (state == nullptr || state->read(&value, sizeof(value), &read) != kResultOk
            || read != sizeof(value))
            return kResultFalse;
        level_ = std::clamp(value, 0.0, 1.0);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) override
    {
        double value = level_;
        return state != nullptr ? state->write(&value, sizeof(value), nullptr) : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 7; }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (IParameterChanges* changes = data.inputParameterChanges)
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                IParamValueQueue* queue = changes->getParameterData(i);
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue != nullptr && queue->getParameterId() == 3 && queue->getPointCount() > 0
                    && queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk)
                    level_ = value;
            }
        if (data.numInputs < 1 || data.numOutputs < 1)
            return kResultOk;
        const auto gain = static_cast<float>(2.0 * level_);
        for (int32 ch = 0; ch < 2; ++ch)
            for (int32 i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[ch][i] = data.inputs[0].channelBuffers32[ch][i] * gain;
        return kResultOk;
    }

    // Controller side.
    tresult PLUGIN_API setComponentState(IBStream* state) override { return setState(state); }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override
    {
        if (index != 0)
            return kInvalidArgument;
        info = {};
        info.id = 3;
        copyName(info.title, "Level");
        copyName(info.shortTitle, "Lvl");
        copyName(info.units, "x");
        info.defaultNormalizedValue = 0.5;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128 string) override
    {
        copyName(string, "?");
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue value) override { return 2.0 * value; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue value) override { return value / 2.0; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return level_; }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    ~Level() override = default;
    double level_ = 0.5;
};

class Keys final : public ProcessorBase, public IConnectionPoint {
public:
    Keys()
        : ProcessorBase(0, 1)
    {
    }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, IComponent::iid) || same(wanted, IPluginBase::iid))
            *obj = static_cast<IComponent*>(this);
        else if (same(wanted, IAudioProcessor::iid))
            *obj = static_cast<IAudioProcessor*>(this);
        else if (same(wanted, IConnectionPoint::iid))
            *obj = static_cast<IConnectionPoint*>(this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    TEST_REFCOUNT

    tresult PLUGIN_API getControllerClassId(TUID classId) override
    {
        std::memcpy(classId, kKeysControllerId, sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    /// Probes the host's stream with positions a careless or hostile
    /// plugin might give: all must be refused. Saves nothing.
    tresult PLUGIN_API getState(IBStream* state) override
    {
        if (state == nullptr)
            return kResultFalse;
        int64 at = 0;
        const bool refused = state->seek(0x7FFFFFFFFFFFFFFF, IBStream::kIBSeekSet, &at) != kResultOk
            && state->seek(-1, IBStream::kIBSeekSet, &at) != kResultOk
            && state->seek(int64{1} << 40, IBStream::kIBSeekCur, &at) != kResultOk;
        if (!refused || state->seek(0, IBStream::kIBSeekSet, &at) != kResultOk)
            return kResultFalse;
        state->seek((int64{1} << 30) - 1, IBStream::kIBSeekSet, &at); // allowed, at the cap
        char byte = 1;
        int32 written = 0;
        if (state->write(&byte, 2, &written) == kResultOk) // would pass the cap
            return kResultFalse;
        return state->seek(0, IBStream::kIBSeekSet, &at); // and leave it empty
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }

    tresult PLUGIN_API connect(IConnectionPoint* other) override
    {
        return other != nullptr ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API notify(IMessage*) override { return kResultOk; }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.numOutputs < 1)
            return kResultOk;
        float* left = data.outputs[0].channelBuffers32[0];
        float* right = data.outputs[0].channelBuffers32[1];
        const ProcessContext* song = data.processContext;
        const bool playing = song != nullptr && (song->state & ProcessContext::kPlaying) != 0;
        const float tempo = song != nullptr ? static_cast<float>(song->tempo) : 0.0f;
        IEventList* events = data.inputEvents;
        const int32 count = events != nullptr ? events->getEventCount() : 0;
        int32 next = 0;
        for (int32 i = 0; i < data.numSamples; ++i) {
            Event e{};
            while (next < count && events->getEvent(next, e) == kResultOk && e.sampleOffset <= i) {
                if (e.type == Event::kNoteOnEvent && e.noteOn.pitch >= 0 && e.noteOn.pitch < 128)
                    held_[static_cast<std::size_t>(e.noteOn.pitch)] = true;
                else if (e.type == Event::kNoteOffEvent && e.noteOff.pitch >= 0 && e.noteOff.pitch < 128)
                    held_[static_cast<std::size_t>(e.noteOff.pitch)] = false;
                ++next;
            }
            const auto down = static_cast<float>(std::count(held_.begin(), held_.end(), true));
            left[i] = 0.1f * down;
            right[i] = playing ? tempo / 1000.0f : 0.0f;
        }
        return kResultOk;
    }

private:
    ~Keys() override = default;
    std::array<bool, 128> held_{};
};

class KeysController final : public IEditController, public IConnectionPoint {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, IEditController::iid)
            || same(wanted, IPluginBase::iid))
            *obj = static_cast<IEditController*>(this);
        else if (same(wanted, IConnectionPoint::iid))
            *obj = static_cast<IConnectionPoint*>(this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    TEST_REFCOUNT

    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 0; }
    tresult PLUGIN_API getParameterInfo(int32, ParameterInfo&) override { return kInvalidArgument; }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kResultFalse; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue value) override { return value; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue value) override { return value; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

    tresult PLUGIN_API connect(IConnectionPoint* other) override
    {
        return other != nullptr ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API notify(IMessage*) override { return kResultOk; }

private:
    ~KeysController() = default;
};

class Factory final : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, IPluginFactory::iid)
            || same(wanted, IPluginFactory2::iid)) {
            *obj = this;
            return kResultOk; // static: no count to keep
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override
    {
        *info = {};
        std::strcpy(info->vendor, "Vloom Tests");
        std::strcpy(info->url, "https://github.com/vloom-audio");
        info->flags = PFactoryInfo::kNoFlags;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 3; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override
    {
        PClassInfo2 full{};
        if (getClassInfo2(index, &full) != kResultOk)
            return kInvalidArgument;
        *info = {};
        std::memcpy(info->cid, full.cid, sizeof(TUID));
        info->cardinality = full.cardinality;
        std::strcpy(info->category, full.category);
        std::strcpy(info->name, full.name);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override
    {
        *info = {};
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->version, "1.0.0");
        std::strcpy(info->sdkVersion, "VST 3.8.1");
        switch (index) {
        case 0:
            std::memcpy(info->cid, kLevelId, sizeof(TUID));
            std::strcpy(info->category, kVstAudioEffectClass);
            std::strcpy(info->name, "Level");
            std::strcpy(info->subCategories, "Fx");
            return kResultOk;
        case 1:
            std::memcpy(info->cid, kKeysId, sizeof(TUID));
            std::strcpy(info->category, kVstAudioEffectClass);
            std::strcpy(info->name, "Keys");
            std::strcpy(info->subCategories, "Instrument|Synth");
            return kResultOk;
        case 2:
            std::memcpy(info->cid, kKeysControllerId, sizeof(TUID));
            std::strcpy(info->category, kVstComponentControllerClass);
            std::strcpy(info->name, "Keys Controller");
            return kResultOk;
        default:
            return kInvalidArgument;
        }
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString wanted, void** obj) override
    {
        FUnknown* made = nullptr;
        if (std::memcmp(cid, kLevelId, sizeof(TUID)) == 0)
            made = static_cast<IComponent*>(new Level);
        else if (std::memcmp(cid, kKeysId, sizeof(TUID)) == 0)
            made = static_cast<IComponent*>(new Keys);
        else if (std::memcmp(cid, kKeysControllerId, sizeof(TUID)) == 0)
            made = static_cast<IEditController*>(new KeysController);
        if (made == nullptr) {
            *obj = nullptr;
            return kNoInterface;
        }
        const tresult result = made->queryInterface(wanted, obj);
        made->release(); // the query holds it now, or nothing does
        return result;
    }
};

Factory factory;

} // namespace

extern "C" {

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() { return &factory; }

#if defined(_WIN32)
SMTG_EXPORT_SYMBOL bool InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll() { return true; }
#elif defined(__APPLE__)
SMTG_EXPORT_SYMBOL bool bundleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit() { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
#endif

} // extern "C"
