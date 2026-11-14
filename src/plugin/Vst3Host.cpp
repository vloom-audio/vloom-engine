// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/Vst3Host.hpp"

#include "plugin/PluginFiles.hpp"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "vloom/engine/time/MusicalTime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <system_error>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace vloom {

namespace {

namespace vst = Steinberg::Vst;
using plugin_files::fail;
using Steinberg::FUnknown;
using Steinberg::int32;
using Steinberg::int64;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::tresult;
using Steinberg::TUID;
using Steinberg::uint32;

bool same(const TUID a, const Steinberg::FUID& b)
{
    return Steinberg::FUnknownPrivate::iidEqual(a, b.toTUID());
}

/// UTF-16 (VST 3 strings) to UTF-8.
std::string utf8(const vst::TChar* text, std::size_t max)
{
    std::string out;
    for (std::size_t i = 0; i < max && text[i] != 0; ++i) {
        std::uint32_t c = static_cast<std::uint16_t>(text[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max) { // a surrogate pair
            const auto low = static_cast<std::uint16_t>(text[i + 1]);
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

/// A fixed-size char array to a string (no terminator needed).
std::string text(const char* chars, std::size_t max)
{
    return {chars, std::find(chars, chars + max, char{})};
}

/// A class id as 32 upper-case hex digits: the plugin's id.
std::string hexId(const TUID cid)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        const auto byte = static_cast<std::uint8_t>(cid[i]);
        out += kDigits[byte >> 4];
        out += kDigits[byte & 0xF];
    }
    return out;
}

bool parseId(std::string_view id, TUID out)
{
    if (id.size() != 32)
        return false;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 16; ++i) {
        const int high = digit(id[2 * i]);
        const int low = digit(id[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        out[i] = static_cast<Steinberg::int8>((high << 4) | low);
    }
    return true;
}

/// Reference counting for host objects that live as long as their owner:
/// the count is kept for plugins that check it, the owner frees them.
#define VLOOM_VST3_OWNED_REFCOUNT                                                                            \
    uint32 PLUGIN_API addRef() override { return ++refs_; }                                                  \
    uint32 PLUGIN_API release() override { return --refs_; }                                                 \
    std::atomic<uint32> refs_{1};

/// Reference counting for host objects handed over to the plugin (messages).
#define VLOOM_VST3_HEAP_REFCOUNT                                                                             \
    uint32 PLUGIN_API addRef() override { return ++refs_; }                                                  \
    uint32 PLUGIN_API release() override                                                                     \
    {                                                                                                        \
        const uint32 left = --refs_;                                                                         \
        if (left == 0)                                                                                       \
            delete this;                                                                                     \
        return left;                                                                                         \
    }                                                                                                        \
    std::atomic<uint32> refs_{1};

/// A key-value list for messages between component and controller.
class AttributeList final : public vst::IAttributeList {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IAttributeList::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_HEAP_REFCOUNT

    tresult PLUGIN_API setInt(AttrID id, int64 value) override
    {
        ints_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override
    {
        const auto it = ints_.find(id);
        if (it == ints_.end())
            return kResultFalse;
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override
    {
        floats_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override
    {
        const auto it = floats_.find(id);
        if (it == floats_.end())
            return kResultFalse;
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const vst::TChar* string) override
    {
        std::u16string& s = strings_[id];
        s.clear();
        for (; string != nullptr && *string != 0; ++string)
            s += static_cast<char16_t>(*string);
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, vst::TChar* string, uint32 sizeInBytes) override
    {
        const auto it = strings_.find(id);
        if (it == strings_.end() || sizeInBytes < sizeof(vst::TChar))
            return kResultFalse;
        const std::size_t room = sizeInBytes / sizeof(vst::TChar) - 1;
        const std::size_t n = std::min(room, it->second.size());
        for (std::size_t i = 0; i < n; ++i)
            string[i] = static_cast<vst::TChar>(it->second[i]);
        string[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) override
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        binaries_[id].assign(bytes, bytes + sizeInBytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) override
    {
        const auto it = binaries_.find(id);
        if (it == binaries_.end())
            return kResultFalse;
        data = it->second.data();
        sizeInBytes = static_cast<uint32>(it->second.size());
        return kResultOk;
    }

private:
    ~AttributeList() = default;
    std::map<std::string, int64, std::less<>> ints_;
    std::map<std::string, double, std::less<>> floats_;
    std::map<std::string, std::u16string, std::less<>> strings_;
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> binaries_;
};

class Message final : public vst::IMessage {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IMessage::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_HEAP_REFCOUNT

    Steinberg::FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(Steinberg::FIDString id) override { id_ = id != nullptr ? id : ""; }
    vst::IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    ~Message() { attributes_->release(); }
    std::string id_;
    AttributeList* attributes_ = new AttributeList;
};

/// What the plugin sees of the host: its name, and messages it may create.
class HostApplication final : public vst::IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IHostApplication::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    tresult PLUGIN_API getName(vst::String128 name) override
    {
        static constexpr char kName[] = "Vloom";
        for (std::size_t i = 0; i < sizeof(kName); ++i)
            name[i] = static_cast<vst::TChar>(kName[i]);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID wanted, void** obj) override
    {
        if (same(cid, vst::IMessage::iid) && same(wanted, vst::IMessage::iid)) {
            *obj = static_cast<vst::IMessage*>(new Message);
            return kResultOk;
        }
        if (same(cid, vst::IAttributeList::iid) && same(wanted, vst::IAttributeList::iid)) {
            *obj = static_cast<vst::IAttributeList*>(new AttributeList);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

/// Hears what the controller tells the host. Without an editor there are
/// no edits to record. restartComponent() is not acted on: latency is read
/// at activation, and a host publishes a new mixer state after
/// re-activating a plugin (ADR 0007).
class ComponentHandler final : public vst::IComponentHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IComponentHandler::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    tresult PLUGIN_API beginEdit(vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(vst::ParamID, vst::ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

/// A byte stream in memory, for state. Positions come from the plugin:
/// they are checked, and the stream never grows past kMaxSize.
class MemoryStream final : public Steinberg::IBStream {
public:
    static constexpr std::int64_t kMaxSize = std::int64_t{1} << 30; ///< 1 GiB

    MemoryStream() = default;
    explicit MemoryStream(std::span<const std::uint8_t> data)
        : data_(data.begin(), data.end())
    {
    }

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, Steinberg::IBStream::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        const std::size_t left = position_ < data_.size() ? data_.size() - position_ : 0;
        const auto n = static_cast<std::size_t>(
            std::clamp<std::int64_t>(numBytes, 0, static_cast<std::int64_t>(left)));
        if (n > 0)
            std::memcpy(buffer, data_.data() + position_, n);
        position_ += n;
        if (numBytesRead != nullptr)
            *numBytesRead = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
    {
        if (numBytes < 0)
            return kInvalidArgument;
        const auto n = static_cast<std::size_t>(numBytes);
        if (static_cast<std::int64_t>(position_) + numBytes > kMaxSize)
            return kResultFalse;
        if (position_ + n > data_.size()) {
            try {
                data_.resize(position_ + n);
            } catch (...) { // never through the plugin's C ABI
                return Steinberg::kOutOfMemory;
            }
        }
        if (n > 0)
            std::memcpy(data_.data() + position_, buffer, n);
        position_ += n;
        if (numBytesWritten != nullptr)
            *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override
    {
        std::int64_t base = 0;
        if (mode == kIBSeekCur)
            base = static_cast<std::int64_t>(position_);
        else if (mode == kIBSeekEnd)
            base = static_cast<std::int64_t>(data_.size());
        // Both are within 0 .. kMaxSize, so this neither overflows nor
        // reaches a position the stream could not grow to.
        if (pos < -base || pos > kMaxSize - base)
            return kInvalidArgument;
        const std::int64_t to = base + pos;
        position_ = static_cast<std::size_t>(to);
        if (result != nullptr)
            *result = to;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override
    {
        if (pos == nullptr)
            return kInvalidArgument;
        *pos = static_cast<int64>(position_);
        return kResultOk;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t position_ = 0;
};

/// One parameter's changes in a block: up to kMaxPoints, fixed storage.
class ParamQueue final : public vst::IParamValueQueue {
public:
    static constexpr int kMaxPoints = 4;

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IParamValueQueue::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    void reset(vst::ParamID id) noexcept
    {
        id_ = id;
        count_ = 0;
    }
    vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return count_; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, vst::ParamValue& value) override
    {
        if (index < 0 || index >= count_)
            return kResultFalse;
        sampleOffset = offsets_[static_cast<std::size_t>(index)];
        value = values_[static_cast<std::size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, vst::ParamValue value, int32& index) override
    {
        if (count_ == kMaxPoints) { // full: the last point takes the newest value
            offsets_[kMaxPoints - 1] = sampleOffset;
            values_[kMaxPoints - 1] = value;
            index = kMaxPoints - 1;
            return kResultOk;
        }
        offsets_[static_cast<std::size_t>(count_)] = sampleOffset;
        values_[static_cast<std::size_t>(count_)] = value;
        index = count_++;
        return kResultOk;
    }

private:
    vst::ParamID id_ = 0;
    int32 count_ = 0;
    std::array<int32, kMaxPoints> offsets_{};
    std::array<vst::ParamValue, kMaxPoints> values_{};
};

/// The block's parameter changes: one queue per parameter, fixed storage.
class ParamChanges final : public vst::IParameterChanges {
public:
    static constexpr int kMaxQueues = 256;

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IParameterChanges::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    void clear() noexcept { count_ = 0; }
    int32 PLUGIN_API getParameterCount() override { return count_; }
    vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        return index >= 0 && index < count_ ? &queues_[static_cast<std::size_t>(index)] : nullptr;
    }
    vst::IParamValueQueue* PLUGIN_API addParameterData(const vst::ParamID& id, int32& index) override
    {
        for (int32 i = 0; i < count_; ++i)
            if (queues_[static_cast<std::size_t>(i)].getParameterId() == id) {
                index = i;
                return &queues_[static_cast<std::size_t>(i)];
            }
        if (count_ == kMaxQueues)
            return nullptr;
        queues_[static_cast<std::size_t>(count_)].reset(id);
        index = count_++;
        return &queues_[static_cast<std::size_t>(index)];
    }

private:
    std::array<ParamQueue, kMaxQueues> queues_{};
    int32 count_ = 0;
};

/// The block's note events, fixed storage.
class EventList final : public vst::IEventList {
public:
    static constexpr int kMaxEvents = 512;

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, vst::IEventList::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    void clear() noexcept { count_ = 0; }
    int32 PLUGIN_API getEventCount() override { return count_; }
    tresult PLUGIN_API getEvent(int32 index, vst::Event& e) override
    {
        if (index < 0 || index >= count_)
            return kResultFalse;
        e = events_[static_cast<std::size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(vst::Event& e) override
    {
        if (count_ == kMaxEvents)
            return kResultFalse;
        events_[static_cast<std::size_t>(count_++)] = e;
        return kResultOk;
    }

private:
    std::array<vst::Event, kMaxEvents> events_{};
    int32 count_ = 0;
};

/// Releases a VST 3 interface pointer and clears it.
template <typename T> void drop(T*& p)
{
    if (p != nullptr)
        p->release();
    p = nullptr;
}

/// One audio bus's buffers, sized at activation.
struct BusBuffers {
    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
};

/// A VST 3 audio processor as an AudioProcessor. The control thread is
/// VST 3's UI thread; process() and reset() run on the audio thread.
class Vst3Processor final : public AudioProcessor {
public:
    static constexpr int32 kMaxBuses = 16;
    static constexpr int32 kMaxBusChannels = 32;

    Vst3Processor(std::shared_ptr<Vst3Library> library, PluginInfo info)
        : library_(std::move(library))
        , info_(std::move(info))
    {
    }

    ~Vst3Processor() override
    {
        Vst3Processor::deactivate();
        if (componentConnection_ != nullptr && controllerConnection_ != nullptr) {
            componentConnection_->disconnect(controllerConnection_);
            controllerConnection_->disconnect(componentConnection_);
        }
        drop(componentConnection_);
        drop(controllerConnection_);
        if (controller_ != nullptr) {
            controller_->setComponentHandler(nullptr);
            if (separateController_)
                controller_->terminate();
        }
        drop(controller_);
        drop(processor_);
        if (component_ != nullptr)
            component_->terminate();
        drop(component_);
    }

    bool create(Steinberg::IPluginFactory& factory, const TUID cid, std::string* error)
    {
        const auto* id = reinterpret_cast<const char*>(cid);
        if (factory.createInstance(id, vst::IComponent::iid.toTUID(), reinterpret_cast<void**>(&component_))
                != kResultOk
            || component_ == nullptr) {
            fail(error, "the plugin " + info_.id + " could not be created");
            return false;
        }
        if (component_->initialize(&host_) != kResultOk) {
            drop(component_);
            fail(error, "the plugin " + info_.id + " failed to initialise");
            return false;
        }
        if (component_->queryInterface(
                vst::IAudioProcessor::iid.toTUID(), reinterpret_cast<void**>(&processor_))
                != kResultOk
            || processor_ == nullptr || processor_->canProcessSampleSize(vst::kSample32) != kResultOk) {
            fail(error, "the plugin " + info_.id + " cannot process 32-bit audio");
            return false;
        }
        // The edit controller: the component itself, or a class of its own.
        if (component_->queryInterface(
                vst::IEditController::iid.toTUID(), reinterpret_cast<void**>(&controller_))
            != kResultOk) {
            controller_ = nullptr;
            TUID controllerId{};
            if (component_->getControllerClassId(controllerId) == kResultOk
                && factory.createInstance(reinterpret_cast<const char*>(controllerId),
                       vst::IEditController::iid.toTUID(), reinterpret_cast<void**>(&controller_))
                    == kResultOk
                && controller_ != nullptr) {
                if (controller_->initialize(&host_) == kResultOk) {
                    separateController_ = true;
                } else {
                    drop(controller_);
                }
            }
        }
        if (controller_ != nullptr) {
            controller_->setComponentHandler(&handler_);
            if (separateController_) {
                component_->queryInterface(
                    vst::IConnectionPoint::iid.toTUID(), reinterpret_cast<void**>(&componentConnection_));
                controller_->queryInterface(
                    vst::IConnectionPoint::iid.toTUID(), reinterpret_cast<void**>(&controllerConnection_));
                if (componentConnection_ != nullptr && controllerConnection_ != nullptr) {
                    componentConnection_->connect(controllerConnection_);
                    controllerConnection_->connect(componentConnection_);
                }
            }
            MemoryStream state;
            if (component_->getState(&state) == kResultOk) {
                state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                controller_->setComponentState(&state);
            }
        }
        return true;
    }

    [[nodiscard]] PluginInfo info() const override { return info_; }

    [[nodiscard]] std::vector<ParameterInfo> parameters() const override
    {
        std::vector<ParameterInfo> list;
        if (controller_ == nullptr)
            return list;
        const int32 count = controller_->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            vst::ParameterInfo p{};
            if (controller_->getParameterInfo(i, p) != kResultOk
                || (p.flags & vst::ParameterInfo::kIsHidden) != 0)
                continue;
            ParameterInfo parameter;
            parameter.id = p.id;
            parameter.name = utf8(p.title, 128);
            parameter.unit = utf8(p.units, 128);
            parameter.minValue = 0.0;
            parameter.maxValue = 1.0;
            parameter.defaultValue = p.defaultNormalizedValue;
            parameter.stepped = p.stepCount > 0;
            list.push_back(std::move(parameter));
        }
        return list;
    }

    bool activate(double sampleRate, int maxBlockFrames) override
    {
        deactivate();
        vst::ProcessSetup setup{vst::kRealtime, vst::kSample32, maxBlockFrames, sampleRate};
        if (processor_->setupProcessing(setup) != kResultOk)
            return false;
        if (!configureBuses(maxBlockFrames))
            return false;
        sampleRate_ = sampleRate;
        if (component_->setActive(true) != kResultOk)
            return false;
        active_ = true;
        processing_ = false;
        held_.fill(false);
        resetPending_ = false;
        return true;
    }

    void deactivate() override
    {
        if (!active_)
            return;
        // As for CLAP: the host calls this only once the audio thread no
        // longer processes the plugin (ADR 0006).
        if (processing_)
            processor_->setProcessing(false);
        processing_ = false;
        component_->setActive(false);
        active_ = false;
    }

    [[nodiscard]] int latencyFrames() const override
    {
        return active_ ? static_cast<int>(std::min<uint32>(processor_->getLatencySamples(), 1u << 30)) : 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> saveState() const override
    {
        // "VLV3", the component's state, the controller's; sizes as 4 bytes, little endian.
        MemoryStream component;
        if (component_->getState(&component) != kResultOk)
            return {};
        MemoryStream controller;
        if (controller_ != nullptr && separateController_ && controller_->getState(&controller) != kResultOk)
            return {};
        std::vector<std::uint8_t> out{'V', 'L', 'V', '3'};
        const auto put = [&out](const std::vector<std::uint8_t>& part) {
            const auto size = static_cast<std::uint32_t>(part.size());
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<std::uint8_t>(size >> shift));
            out.insert(out.end(), part.begin(), part.end());
        };
        put(component.data());
        put(controller.data());
        return out;
    }

    bool loadState(std::span<const std::uint8_t> data) override
    {
        std::size_t at = 4;
        if (data.size() < 4 || std::memcmp(data.data(), "VLV3", 4) != 0)
            return false;
        const auto take = [&](std::span<const std::uint8_t>& part) {
            if (data.size() - at < 4)
                return false;
            std::uint32_t size = 0;
            for (int i = 0; i < 4; ++i)
                size |= static_cast<std::uint32_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
            at += 4;
            if (data.size() - at < size)
                return false;
            part = data.subspan(at, size);
            at += size;
            return true;
        };
        std::span<const std::uint8_t> componentState;
        std::span<const std::uint8_t> controllerState;
        if (!take(componentState) || !take(controllerState))
            return false;
        MemoryStream component(componentState);
        if (component_->setState(&component) != kResultOk)
            return false;
        if (controller_ != nullptr) {
            MemoryStream again(componentState);
            controller_->setComponentState(&again);
            if (separateController_ && !controllerState.empty()) {
                MemoryStream controller(controllerState);
                controller_->setState(&controller);
            }
        }
        return true;
    }

    void process(const ProcessContext& context) noexcept override
    {
        if (!active_ || context.numFrames <= 0 || context.numChannels < 1)
            return;
        if (!processing_) {
            processor_->setProcessing(true); // kNotImplemented is fine too
            processing_ = true;
        }
        const auto frames = static_cast<std::size_t>(context.numFrames);
        float* left = context.channels[0];
        float* right = context.numChannels > 1 ? context.channels[1] : context.channels[0];

        // Parameters: their latest value at their frame.
        changes_.clear();
        for (const ParameterEvent& p : context.parameters) {
            int32 index = 0;
            if (vst::IParamValueQueue* queue = changes_.addParameterData(p.id, index); queue != nullptr)
                queue->addPoint(std::max(p.frame, 0), std::clamp(p.value, 0.0, 1.0), index);
        }
        // Notes: after a reset, every held key ends first.
        events_.clear();
        const auto note = [&](int frame, bool on, int key, float velocity) {
            vst::Event e{};
            e.busIndex = 0;
            e.sampleOffset = std::max(frame, 0);
            if (on) {
                e.type = vst::Event::kNoteOnEvent;
                e.noteOn.channel = 0;
                e.noteOn.pitch = static_cast<Steinberg::int16>(key);
                e.noteOn.velocity = velocity;
                e.noteOn.noteId = -1;
            } else {
                e.type = vst::Event::kNoteOffEvent;
                e.noteOff.channel = 0;
                e.noteOff.pitch = static_cast<Steinberg::int16>(key);
                e.noteOff.velocity = velocity;
                e.noteOff.noteId = -1;
            }
            if (events_.addEvent(e) == kResultOk)
                held_[static_cast<std::size_t>(key)] = on;
        };
        if (resetPending_) {
            resetPending_ = false;
            for (int key = 0; key < 128; ++key)
                if (held_[static_cast<std::size_t>(key)])
                    note(0, false, key, 0.0f);
        }
        for (const NoteEvent& n : context.notes)
            if (n.key >= 0 && n.key < 128)
                note(n.frame, n.type == NoteEvent::Type::On, n.key, std::clamp(n.velocity, 0.0f, 1.0f));

        // Audio: ours into the main input; every other buffer silent.
        for (std::size_t bus = 0; bus < inputs_.size(); ++bus)
            for (std::size_t ch = 0; ch < inputs_[bus].pointers.size(); ++ch) {
                float* dst = inputs_[bus].pointers[ch];
                if (bus != 0 || ch >= 2)
                    std::fill_n(dst, frames, 0.0f);
                else if (inputs_[bus].pointers.size() == 1)
                    for (std::size_t i = 0; i < frames; ++i)
                        dst[i] = 0.5f * (left[i] + right[i]);
                else
                    std::copy_n(ch == 0 ? left : right, frames, dst);
            }
        for (BusBuffers& bus : outputs_)
            for (float* dst : bus.pointers)
                std::fill_n(dst, frames, 0.0f);

        vst::ProcessContext song{};
        song.state = static_cast<uint32>(vst::ProcessContext::kTempoValid)
            | static_cast<uint32>(vst::ProcessContext::kProjectTimeMusicValid)
            | (context.transport.playing ? static_cast<uint32>(vst::ProcessContext::kPlaying) : 0u);
        song.sampleRate = sampleRate_;
        song.tempo = context.transport.tempo;
        song.projectTimeMusic = context.transport.positionTicks / kPpq;
        // In samples at the current tempo: exact while the tempo stays the
        // same (TransportInfo carries no tempo map). No time signature is
        // given: the transport does not carry it either.
        song.projectTimeSamples = context.transport.tempo > 0.0
            ? static_cast<vst::TSamples>(song.projectTimeMusic * 60.0 / context.transport.tempo * sampleRate_)
            : 0;

        for (std::size_t bus = 0; bus < inputs_.size(); ++bus) {
            inputBuffers_[bus].numChannels = static_cast<int32>(inputs_[bus].pointers.size());
            inputBuffers_[bus].silenceFlags = 0;
            inputBuffers_[bus].channelBuffers32 = inputs_[bus].pointers.data();
        }
        for (std::size_t bus = 0; bus < outputs_.size(); ++bus) {
            outputBuffers_[bus].numChannels = static_cast<int32>(outputs_[bus].pointers.size());
            outputBuffers_[bus].silenceFlags = 0;
            outputBuffers_[bus].channelBuffers32 = outputs_[bus].pointers.data();
        }
        vst::ProcessData data;
        data.processMode = vst::kRealtime;
        data.symbolicSampleSize = vst::kSample32;
        data.numSamples = context.numFrames;
        data.numInputs = static_cast<int32>(inputBuffers_.size());
        data.numOutputs = static_cast<int32>(outputBuffers_.size());
        data.inputs = inputBuffers_.empty() ? nullptr : inputBuffers_.data();
        data.outputs = outputBuffers_.empty() ? nullptr : outputBuffers_.data();
        data.inputParameterChanges = &changes_;
        data.inputEvents = hasEventInput_ ? &events_ : nullptr;
        data.processContext = &song;
        if (processor_->process(data) != kResultOk || outputs_.empty())
            return; // the audio stays as it came

        // The main output back. A plugin with no audio input (an
        // instrument) adds to what is there; an effect replaces it.
        const BusBuffers& out = outputs_[0];
        if (out.pointers.empty())
            return;
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
        // VST 3 has no reset: end every held note in the next block.
        resetPending_ = true;
    }

private:
    /// Stereo main buses where the plugin agrees; buffers for every bus.
    bool configureBuses(int maxBlockFrames)
    {
        const int32 inCount = component_->getBusCount(vst::kAudio, vst::kInput);
        const int32 outCount = component_->getBusCount(vst::kAudio, vst::kOutput);
        if (inCount > kMaxBuses || outCount > kMaxBuses || inCount < 0 || outCount < 0)
            return false;
        const auto arrangements = [&](int32 count, vst::BusDirection direction) {
            std::vector<vst::SpeakerArrangement> list(
                static_cast<std::size_t>(count), vst::SpeakerArr::kStereo);
            for (int32 i = 1; i < count; ++i)
                processor_->getBusArrangement(direction, i, list[static_cast<std::size_t>(i)]);
            return list;
        };
        auto ins = arrangements(inCount, vst::kInput);
        auto outs = arrangements(outCount, vst::kOutput);
        processor_->setBusArrangements(ins.empty() ? nullptr : ins.data(), inCount,
            outs.empty() ? nullptr : outs.data(), outCount); // refused: what the plugin has stays
        const auto size = [&](int32 count, vst::BusDirection direction, std::vector<BusBuffers>& buses,
                              std::vector<vst::AudioBusBuffers>& buffers) {
            buses.clear();
            buffers.assign(static_cast<std::size_t>(count), vst::AudioBusBuffers{});
            for (int32 i = 0; i < count; ++i) {
                vst::SpeakerArrangement arrangement = 0;
                processor_->getBusArrangement(direction, i, arrangement);
                const int channels = std::popcount(arrangement);
                if (channels > kMaxBusChannels)
                    return false;
                BusBuffers bus;
                bus.channels.assign(static_cast<std::size_t>(channels),
                    std::vector<float>(static_cast<std::size_t>(maxBlockFrames), 0.0f));
                for (auto& channel : bus.channels)
                    bus.pointers.push_back(channel.data());
                buses.push_back(std::move(bus));
                component_->activateBus(vst::kAudio, direction, i, i == 0);
            }
            return true;
        };
        if (!size(inCount, vst::kInput, inputs_, inputBuffers_)
            || !size(outCount, vst::kOutput, outputs_, outputBuffers_))
            return false;
        hasEventInput_ = component_->getBusCount(vst::kEvent, vst::kInput) > 0;
        if (hasEventInput_)
            component_->activateBus(vst::kEvent, vst::kInput, 0, true);
        return true;
    }

    std::shared_ptr<Vst3Library> library_; ///< kept loaded while the plugin lives
    PluginInfo info_;
    HostApplication host_;
    ComponentHandler handler_;
    vst::IComponent* component_ = nullptr;
    vst::IAudioProcessor* processor_ = nullptr;
    vst::IEditController* controller_ = nullptr;
    bool separateController_ = false;
    vst::IConnectionPoint* componentConnection_ = nullptr;
    vst::IConnectionPoint* controllerConnection_ = nullptr;

    bool active_ = false;
    bool processing_ = false; ///< audio thread, once active
    bool hasEventInput_ = false;
    bool resetPending_ = false; ///< audio thread
    double sampleRate_ = 48000.0;
    std::array<bool, 128> held_{}; ///< audio thread: keys it was told are down

    std::vector<BusBuffers> inputs_, outputs_;
    std::vector<vst::AudioBusBuffers> inputBuffers_, outputBuffers_;
    ParamChanges changes_;
    EventList events_;
};

using GetFactory = Steinberg::IPluginFactory*(PLUGIN_API*)();

/// A library symbol as a function pointer (without a cast between object
/// and function pointers, which ISO C++ leaves to the implementation).
template <typename Function> Function functionAt(void* symbol) noexcept
{
    static_assert(sizeof(Function) == sizeof(void*));
    Function f = nullptr;
    std::memcpy(&f, &symbol, sizeof(f));
    return f;
}

/// The binary inside a .vst3: the file itself, or the one for this OS and
/// architecture inside the bundle directory.
std::filesystem::path binaryOf(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec))
        return path;
    const std::filesystem::path contents = path / "Contents";
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return contents / "arm64-win" / path.filename();
#else
    return contents / "x86_64-win" / path.filename();
#endif
#elif defined(__APPLE__)
    return contents / "MacOS" / path.stem();
#else
#if defined(__aarch64__)
    return contents / "aarch64-linux" / (path.stem().string() + ".so");
#else
    return contents / "x86_64-linux" / (path.stem().string() + ".so");
#endif
#endif
}

std::mutex& cacheMutex()
{
    static std::mutex m;
    return m;
}

std::map<std::filesystem::path, std::weak_ptr<Vst3Library>>& cache()
{
    static std::map<std::filesystem::path, std::weak_ptr<Vst3Library>> c;
    return c;
}

} // namespace

struct Vst3Library::Impl {
    std::filesystem::path path;
    plugin_files::SharedLibrary module;
    Steinberg::IPluginFactory* factory = nullptr;
    std::vector<PluginInfo> plugins;
    std::vector<std::array<Steinberg::int8, 16>> classIds; ///< per plugin
    bool entered = false; ///< the module's entry function ran: its exit runs too
#if defined(__APPLE__)
    CFBundleRef bundle = nullptr;
#endif

    ~Impl()
    {
        drop(factory);
        if (!entered)
            return;
#if defined(_WIN32)
        if (auto exitDll = functionAt<bool (*)()>(module.symbol("ExitDll")))
            exitDll();
#elif defined(__APPLE__)
        if (auto bundleExit = functionAt<bool (*)()>(module.symbol("bundleExit")))
            bundleExit();
        if (bundle != nullptr)
            CFRelease(bundle);
#else
        if (auto moduleExit = functionAt<bool (*)()>(module.symbol("ModuleExit")))
            moduleExit();
#endif
    }

    /// Runs the module's entry function, if it has one.
    bool enter()
    {
#if defined(_WIN32)
        auto initDll = functionAt<bool (*)()>(module.symbol("InitDll"));
        entered = true;
        return initDll == nullptr || initDll();
#elif defined(__APPLE__)
        auto bundleEntry = functionAt<bool (*)(CFBundleRef)>(module.symbol("bundleEntry"));
        if (bundleEntry == nullptr)
            return true;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            const std::string p = path.string();
            if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
                    reinterpret_cast<const UInt8*>(p.c_str()), static_cast<CFIndex>(p.size()), true)) {
                bundle = CFBundleCreate(nullptr, url);
                CFRelease(url);
            }
        }
        entered = true;
        return bundleEntry(bundle);
#else
        auto moduleEntry = functionAt<bool (*)(void*)>(module.symbol("ModuleEntry"));
        entered = true;
        return moduleEntry == nullptr || moduleEntry(module.handle());
#endif
    }
};

Vst3Library::Vst3Library(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

Vst3Library::~Vst3Library() = default;

std::shared_ptr<Vst3Library> Vst3Library::open(const std::filesystem::path& path, std::string* error)
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
    auto getFactory = functionAt<GetFactory>(impl->module.symbol("GetPluginFactory"));
    if (getFactory == nullptr) {
        fail(error, key.string() + " is not a VST 3 plugin (no GetPluginFactory)");
        return nullptr;
    }
    if (!impl->enter()) {
        fail(error, key.string() + " failed to initialise");
        return nullptr;
    }
    impl->factory = getFactory();
    if (impl->factory == nullptr) {
        fail(error, key.string() + " has no plugin factory");
        return nullptr;
    }
    Steinberg::PFactoryInfo factoryInfo{};
    impl->factory->getFactoryInfo(&factoryInfo);
    Steinberg::IPluginFactory2* factory2 = nullptr;
    if (impl->factory->queryInterface(
            Steinberg::IPluginFactory2::iid.toTUID(), reinterpret_cast<void**>(&factory2))
        != kResultOk)
        factory2 = nullptr;
    const int32 count = impl->factory->countClasses();
    for (int32 i = 0; i < count; ++i) {
        Steinberg::PClassInfo info{};
        if (impl->factory->getClassInfo(i, &info) != kResultOk
            || text(info.category, sizeof(info.category)) != kVstAudioEffectClass)
            continue;
        PluginInfo plugin;
        plugin.id = hexId(info.cid);
        plugin.name = text(info.name, sizeof(info.name));
        plugin.vendor = text(factoryInfo.vendor, sizeof(factoryInfo.vendor));
        Steinberg::PClassInfo2 info2{};
        if (factory2 != nullptr && factory2->getClassInfo2(i, &info2) == kResultOk) {
            if (const std::string vendor = text(info2.vendor, sizeof(info2.vendor)); !vendor.empty())
                plugin.vendor = vendor;
            plugin.version = text(info2.version, sizeof(info2.version));
            // Sub-categories, '|'-separated: "Instrument", "Instrument|Synth" ...
            const std::string sub = text(info2.subCategories, sizeof(info2.subCategories));
            if (sub.rfind("Instrument", 0) == 0 || sub.find("|Instrument") != std::string::npos)
                plugin.kind = PluginInfo::Kind::Instrument;
        }
        std::array<Steinberg::int8, 16> cid{};
        std::memcpy(cid.data(), info.cid, 16);
        impl->classIds.push_back(cid);
        impl->plugins.push_back(std::move(plugin));
    }
    drop(factory2);
    std::shared_ptr<Vst3Library> library(new Vst3Library(std::move(impl)));
    cache()[key] = library;
    return library;
}

const std::filesystem::path& Vst3Library::path() const noexcept { return impl_->path; }

const std::vector<PluginInfo>& Vst3Library::plugins() const noexcept { return impl_->plugins; }

std::unique_ptr<AudioProcessor> Vst3Library::create(std::string_view id, std::string* error)
{
    TUID wanted{};
    const auto it = std::find_if(
        impl_->plugins.begin(), impl_->plugins.end(), [&](const PluginInfo& p) { return p.id == id; });
    if (it == impl_->plugins.end() || !parseId(id, wanted)) {
        fail(error, "no plugin " + std::string(id) + " in " + impl_->path.string());
        return nullptr;
    }
    auto processor = std::make_unique<Vst3Processor>(shared_from_this(), *it);
    if (!processor->create(*impl_->factory, wanted, error))
        return nullptr;
    return processor;
}

std::vector<std::filesystem::path> vst3SearchPaths()
{
    std::vector<std::filesystem::path> paths = plugin_files::pathsFrom("VST3_PATH");
    const auto environment = plugin_files::environment;
#if defined(_WIN32)
    if (const std::string common = environment("COMMONPROGRAMFILES"); !common.empty())
        paths.push_back(std::filesystem::path(common) / "VST3");
    if (const std::string local = environment("LOCALAPPDATA"); !local.empty())
        paths.push_back(std::filesystem::path(local) / "Programs" / "Common" / "VST3");
#elif defined(__APPLE__)
    paths.emplace_back("/Library/Audio/Plug-Ins/VST3");
    if (const std::string home = environment("HOME"); !home.empty())
        paths.push_back(std::filesystem::path(home) / "Library" / "Audio" / "Plug-Ins" / "VST3");
#else
    if (const std::string home = environment("HOME"); !home.empty())
        paths.push_back(std::filesystem::path(home) / ".vst3");
    paths.emplace_back("/usr/lib/vst3");
    paths.emplace_back("/usr/local/lib/vst3");
#endif
    return paths;
}

std::vector<std::filesystem::path> findVst3Files(const std::vector<std::filesystem::path>& directories)
{
    return plugin_files::findFiles(directories, ".vst3");
}

} // namespace vloom
