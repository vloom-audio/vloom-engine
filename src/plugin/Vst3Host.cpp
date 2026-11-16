// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/plugin/Vst3Host.hpp"

#include "plugin/EditorRunLoop.hpp"
#include "plugin/PluginFiles.hpp"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "vloom/engine/core/SpscQueue.hpp"
#include "vloom/engine/plugin/PluginInstance.hpp"
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

/// The platform type editors are opened with (ADR 0009).
#if defined(_WIN32)
const Steinberg::FIDString kPlatformType = Steinberg::kPlatformTypeHWND;
#elif defined(__APPLE__)
const Steinberg::FIDString kPlatformType = Steinberg::kPlatformTypeNSView;
#else
const Steinberg::FIDString kPlatformType = Steinberg::kPlatformTypeX11EmbedWindowID;
#endif

/// What the host objects below report to the processor that owns them.
class EditorOwner {
public:
    /// The controller changed a parameter (an edit in its editor), UI thread.
    virtual void edited(vst::ParamID id, vst::ParamValue value) = 0;
    /// The editor asks to be resized.
    virtual tresult resizeRequested(Steinberg::IPlugView* view, Steinberg::ViewRect* size) = 0;
    virtual plugin_editor::RunLoop& runLoop() = 0;

protected:
    ~EditorOwner() = default;
};

/// Hears what the controller tells the host: performEdit() is an edit in the
/// editor, which goes to the processor (ADR 0009). restartComponent() is not
/// acted on: latency is read at activation, and a host publishes a new mixer
/// state after re-activating a plugin (ADR 0007).
class ComponentHandler final : public vst::IComponentHandler {
public:
    explicit ComponentHandler(EditorOwner& owner)
        : owner_(owner)
    {
    }

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
    tresult PLUGIN_API performEdit(vst::ParamID id, vst::ParamValue value) override
    {
        if (!std::isfinite(value))
            return kInvalidArgument;
        owner_.edited(id, std::clamp(value, 0.0, 1.0));
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

private:
    EditorOwner& owner_;
};

/// The window around an editor: it hears resize requests, and on Linux it
/// is the run loop for the editor's timers and file descriptors.
class PlugFrame final : public Steinberg::IPlugFrame
#if defined(__linux__)
    ,
                        public Steinberg::Linux::IRunLoop
#endif
{
public:
    explicit PlugFrame(EditorOwner& owner)
        : owner_(owner)
    {
    }
#if defined(__linux__)
    ~PlugFrame()
    {
        for (auto& [handler, fd] : fdHandlers_)
            handler->release();
        for (auto& [handler, id] : timerHandlers_)
            handler->release();
    }
#endif
    PlugFrame(const PlugFrame&) = delete;
    PlugFrame& operator=(const PlugFrame&) = delete;

    tresult PLUGIN_API queryInterface(const TUID wanted, void** obj) override
    {
        if (same(wanted, FUnknown::iid) || same(wanted, Steinberg::IPlugFrame::iid)) {
            addRef();
            *obj = static_cast<Steinberg::IPlugFrame*>(this);
            return kResultOk;
        }
#if defined(__linux__)
        if (same(wanted, Steinberg::Linux::IRunLoop::iid)) {
            addRef();
            *obj = static_cast<Steinberg::Linux::IRunLoop*>(this);
            return kResultOk;
        }
#endif
        *obj = nullptr;
        return kNoInterface;
    }
    VLOOM_VST3_OWNED_REFCOUNT

    tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* size) override
    {
        return owner_.resizeRequested(view, size);
    }

#if defined(__linux__)
    tresult PLUGIN_API registerEventHandler(
        Steinberg::Linux::IEventHandler* handler, Steinberg::Linux::FileDescriptor fd) override
    {
        if (handler == nullptr
            || !owner_.runLoop().addFd(
                fd, plugin_editor::RunLoop::kRead, [handler, fd](std::uint32_t) { handler->onFDIsSet(fd); }))
            return kResultFalse;
        handler->addRef();
        fdHandlers_.emplace_back(handler, fd);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler* handler) override
    {
        return removeAll(fdHandlers_, handler, [this](int fd) { owner_.runLoop().removeFd(fd); });
    }
    tresult PLUGIN_API registerTimer(
        Steinberg::Linux::ITimerHandler* handler, Steinberg::Linux::TimerInterval interval) override
    {
        if (handler == nullptr)
            return kResultFalse;
        const auto period = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(std::min<Steinberg::uint64>(interval, 3'600'000))};
        const std::uint32_t id
            = owner_.runLoop().addTimer(period, [handler](std::uint32_t) { handler->onTimer(); });
        if (id == 0)
            return kResultFalse;
        handler->addRef();
        timerHandlers_.emplace_back(handler, id);
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* handler) override
    {
        return removeAll(
            timerHandlers_, handler, [this](std::uint32_t id) { owner_.runLoop().removeTimer(id); });
    }
#endif

private:
#if defined(__linux__)
    template <typename Handler, typename Key, typename Remove>
    static tresult removeAll(std::vector<std::pair<Handler*, Key>>& list, Handler* handler, Remove remove)
    {
        bool found = false;
        for (auto it = list.begin(); it != list.end();)
            if (it->first == handler) {
                remove(it->second);
                it->first->release();
                it = list.erase(it);
                found = true;
            } else {
                ++it;
            }
        return found ? kResultOk : kResultFalse;
    }

    std::vector<std::pair<Steinberg::Linux::IEventHandler*, int>> fdHandlers_;
    std::vector<std::pair<Steinberg::Linux::ITimerHandler*, std::uint32_t>> timerHandlers_;
#endif
    EditorOwner& owner_;
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
class Vst3Processor final : public AudioProcessor, private EditorOwner {
public:
    /// Per direction. Samplers and drum machines have one bus per output
    /// pair (Battery: 32, Kontakt: up to 64); this only bounds the memory.
    static constexpr int32 kMaxBuses = 128;
    static constexpr int32 kMaxBusChannels = 32;

    Vst3Processor(std::shared_ptr<Vst3Library> library, PluginInfo info)
        : library_(std::move(library))
        , info_(std::move(info))
    {
    }

    ~Vst3Processor() override
    {
        Vst3Processor::closeEditor();
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
        drop(units_);
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
            // Which of its parameters a pitch bend, a controller or
            // pressure moves: VST 3 has no events for those. Asked once,
            // here: the audio thread must not call the controller.
            vst::IMidiMapping* mapping = nullptr;
            if (controller_->queryInterface(
                    vst::IMidiMapping::iid.toTUID(), reinterpret_cast<void**>(&mapping))
                    == kResultOk
                && mapping != nullptr) {
                // For every channel: an MPE plugin maps the controls of each to another parameter.
                for (std::size_t channel = 0; channel < controlParameter_.size(); ++channel)
                    for (std::size_t control = 0; control < controlParameter_[channel].size(); ++control) {
                        vst::ParamID moved = 0;
                        controlMapped_[channel][control]
                            = mapping->getMidiControllerAssignment(0, static_cast<Steinberg::int16>(channel),
                                  static_cast<vst::CtrlNumber>(control), moved)
                            == kResultOk;
                        controlParameter_[channel][control] = moved;
                    }
                mapping->release();
            }
            if (controller_->queryInterface(vst::IUnitInfo::iid.toTUID(), reinterpret_cast<void**>(&units_))
                != kResultOk)
                units_ = nullptr;
            // The parameter that switches programs, if it has one.
            const int32 count = controller_->getParameterCount();
            for (int32 i = 0; i < count && !hasProgramParameter_; ++i) {
                vst::ParameterInfo p{};
                if (controller_->getParameterInfo(i, p) == kResultOk
                    && (p.flags & vst::ParameterInfo::kIsProgramChange) != 0) {
                    hasProgramParameter_ = true;
                    programParameter_ = p.id;
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
        // VST 3 order: bus arrangements, then setupProcessing, then setActive.
        if (!configureBuses(maxBlockFrames))
            return false;
        vst::ProcessSetup setup{vst::kRealtime, vst::kSample32, maxBlockFrames, sampleRate};
        if (processor_->setupProcessing(setup) != kResultOk)
            return false;
        sampleRate_ = sampleRate;
        if (component_->setActive(true) != kResultOk)
            return false;
        active_ = true;
        processing_ = false;
        held_.fill(0);
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

    [[nodiscard]] int extraOutputCount() const override { return extraOutputs_; }
    [[nodiscard]] bool hasSidechain() const override { return hasSidechain_; }
    [[nodiscard]] bool makesNotes() const override { return hasEventOutput_; }

    [[nodiscard]] std::vector<std::string> programs() const override
    {
        std::vector<std::string> names;
        vst::ProgramListInfo list{};
        if (units_ == nullptr || !hasProgramParameter_ || units_->getProgramListCount() < 1
            || units_->getProgramListInfo(0, list) != kResultOk)
            return names;
        const int32 count = std::clamp<int32>(list.programCount, 0, kMaxPrograms);
        for (int32 i = 0; i < count; ++i) {
            vst::String128 name{};
            if (units_->getProgramName(list.id, i, name) != kResultOk)
                name[0] = 0;
            names.push_back(utf8(name, 128));
            if (names.back().empty())
                names.back() = "Program " + std::to_string(i + 1);
        }
        return names;
    }

    [[nodiscard]] int currentProgram() const override
    {
        const int count = programCount();
        if (count < 1)
            return -1;
        const double value = controller_->getParamNormalized(programParameter_);
        return std::isfinite(value)
            ? std::clamp(static_cast<int>(std::lround(value * (count - 1))), 0, count - 1)
            : -1;
    }

    bool setProgram(int index) override
    {
        const int count = programCount();
        if (index < 0 || index >= count)
            return false;
        const double value = count > 1 ? static_cast<double>(index) / (count - 1) : 0.0;
        controller_->setParamNormalized(programParameter_, value);
        return edits_.push(Edit{programParameter_, value}); // to the processor with its next block
    }

    [[nodiscard]] int latencyFrames() const override
    {
        return active_ ? static_cast<int>(std::min<uint32>(processor_->getLatencySamples(), 1u << 30)) : 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> saveState() const override
    {
        // "VLV3", the component's state, the controller's; sizes as 4 bytes,
        // little endian. Either part may be missing: many plugins keep
        // everything in one of the two and refuse to write the other, and a
        // refusal there must not lose what the other one wrote.
        MemoryStream component;
        const bool haveComponent = component_->getState(&component) == kResultOk;
        MemoryStream controller;
        const bool haveController = controller_ != nullptr && separateController_
            && controller_->getState(&controller) == kResultOk;
        static const std::vector<std::uint8_t> kNothing;
        const std::vector<std::uint8_t>& componentData = haveComponent ? component.data() : kNothing;
        const std::vector<std::uint8_t>& controllerData = haveController ? controller.data() : kNothing;
        if (!haveComponent && !haveController)
            return {};
        std::vector<std::uint8_t> out{'V', 'L', 'V', '3'};
        const auto put = [&out](const std::vector<std::uint8_t>& part) {
            const auto size = static_cast<std::uint32_t>(part.size());
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<std::uint8_t>(size >> shift));
            out.insert(out.end(), part.begin(), part.end());
        };
        put(componentData);
        put(controllerData);
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
        // Each part goes to where it came from; one that was not saved is
        // not given. True if the plugin took at least one (or there was
        // nothing to take).
        bool taken = componentState.empty() && controllerState.empty();
        if (!componentState.empty()) {
            MemoryStream component(componentState);
            taken = component_->setState(&component) == kResultOk;
            if (controller_ != nullptr) {
                MemoryStream again(componentState);
                controller_->setComponentState(&again);
            }
        }
        if (controller_ != nullptr && separateController_ && !controllerState.empty()) {
            MemoryStream controller(controllerState);
            taken = controller_->setState(&controller) == kResultOk || taken;
        }
        return taken;
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

        // Parameters: edits in the editor first, then the host's changes at
        // their frame (a later point wins).
        changes_.clear();
        Edit edit;
        while (edits_.pop(edit)) {
            int32 index = 0;
            if (vst::IParamValueQueue* queue = changes_.addParameterData(edit.id, index); queue != nullptr)
                queue->addPoint(0, edit.value, index);
        }
        for (const ParameterEvent& p : context.parameters) {
            int32 index = 0;
            if (vst::IParamValueQueue* queue = changes_.addParameterData(p.id, index); queue != nullptr)
                queue->addPoint(std::max(p.frame, 0), std::clamp(p.value, 0.0, 1.0), index);
        }
        // Controls: as changes of the parameters the plugin mapped them to.
        for (const ControlEvent& control : context.controls) {
            const auto onChannel = static_cast<std::size_t>(std::clamp<int>(control.channel, 0, 15));
            const std::size_t which = control.type == ControlEvent::Type::PitchBend ? 129u
                : control.type == ControlEvent::Type::Pressure
                ? 128u
                : static_cast<std::size_t>(std::clamp<int>(control.number, 0, 127));
            if (!controlMapped_[onChannel][which] || !std::isfinite(control.value))
                continue;
            const double value = control.type == ControlEvent::Type::PitchBend
                ? 0.5 * (static_cast<double>(control.value) + 1.0)
                : static_cast<double>(control.value);
            int32 index = 0;
            if (vst::IParamValueQueue* queue
                = changes_.addParameterData(controlParameter_[onChannel][which], index);
                queue != nullptr)
                queue->addPoint(std::max(control.frame, 0), std::clamp(value, 0.0, 1.0), index);
        }
        // Notes: after a reset, every held key ends first.
        events_.clear();
        const auto note = [&](int frame, bool on, int key, float velocity, int channel) {
            vst::Event e{};
            e.busIndex = 0;
            e.sampleOffset = std::max(frame, 0);
            if (on) {
                e.type = vst::Event::kNoteOnEvent;
                e.noteOn.channel = static_cast<Steinberg::int16>(channel);
                e.noteOn.pitch = static_cast<Steinberg::int16>(key);
                e.noteOn.velocity = velocity;
                e.noteOn.noteId = -1;
            } else {
                e.type = vst::Event::kNoteOffEvent;
                e.noteOff.channel = static_cast<Steinberg::int16>(channel);
                e.noteOff.pitch = static_cast<Steinberg::int16>(key);
                e.noteOff.velocity = velocity;
                e.noteOff.noteId = -1;
            }
            if (events_.addEvent(e) == kResultOk) {
                std::uint16_t& channels = held_[static_cast<std::size_t>(key)];
                channels = static_cast<std::uint16_t>(
                    on ? channels | (1u << channel) : channels & ~(1u << channel));
            }
        };
        if (resetPending_) {
            resetPending_ = false;
            for (int key = 0; key < 128; ++key)
                for (int channel = 0; channel < 16; ++channel)
                    if (((held_[static_cast<std::size_t>(key)] >> channel) & 1u) != 0)
                        note(0, false, key, 0.0f, channel);
        }
        for (const NoteEvent& n : context.notes)
            if (n.key >= 0 && n.key < 128)
                note(n.frame, n.type == NoteEvent::Type::On, n.key, std::clamp(n.velocity, 0.0f, 1.0f),
                    std::clamp<int>(n.channel, 0, 15));

        // Audio: ours into the main input; every other buffer silent.
        for (std::size_t bus = 0; bus < inputs_.size(); ++bus)
            for (std::size_t ch = 0; ch < inputs_[bus].pointers.size(); ++ch) {
                float* dst = inputs_[bus].pointers[ch];
                if (bus == 1 && hasSidechain_ && context.sidechain != nullptr && ch < 2
                    && inputs_[bus].pointers.size() > 1)
                    std::copy_n(context.sidechain[ch], frames, dst);
                else if (bus == 1 && hasSidechain_ && context.sidechain != nullptr
                    && inputs_[bus].pointers.size() == 1)
                    for (std::size_t i = 0; i < frames; ++i)
                        dst[i] = 0.5f * (context.sidechain[0][i] + context.sidechain[1][i]);
                else if (bus != 0 || ch >= 2)
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
        // same (TransportInfo carries no tempo map).
        song.projectTimeSamples = context.transport.tempo > 0.0
            ? static_cast<vst::TSamples>(song.projectTimeMusic * 60.0 / context.transport.tempo * sampleRate_)
            : 0;
        if (context.transport.beatsPerBar > 0 && context.transport.beatUnit > 0) {
            const double barBeats = context.transport.beatsPerBar * 4.0 / context.transport.beatUnit;
            song.timeSigNumerator = context.transport.beatsPerBar;
            song.timeSigDenominator = context.transport.beatUnit;
            song.barPositionMusic = std::floor(song.projectTimeMusic / barBeats) * barBeats;
            song.state |= static_cast<uint32>(vst::ProcessContext::kTimeSigValid)
                | static_cast<uint32>(vst::ProcessContext::kBarPositionValid);
        }
        if (context.transport.loopEndTicks > context.transport.loopStartTicks) {
            song.cycleStartMusic = context.transport.loopStartTicks / kPpq;
            song.cycleEndMusic = context.transport.loopEndTicks / kPpq;
            song.state |= static_cast<uint32>(vst::ProcessContext::kCycleValid)
                | static_cast<uint32>(vst::ProcessContext::kCycleActive);
        }

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
        madeEvents_.clear();
        data.outputEvents = hasEventOutput_ ? &madeEvents_ : nullptr;
        data.processContext = &song;
        if (processor_->process(data) != kResultOk)
            return; // the audio stays as it came
        // The notes it made (an arpeggiator), for the host to pass on.
        if (context.notesOutCount != nullptr) {
            std::size_t made = 0;
            const int32 count = madeEvents_.getEventCount();
            for (int32 i = 0; i < count && made < context.notesOut.size(); ++i) {
                vst::Event e{};
                if (madeEvents_.getEvent(i, e) != kResultOk
                    || (e.type != vst::Event::kNoteOnEvent && e.type != vst::Event::kNoteOffEvent))
                    continue;
                const bool on = e.type == vst::Event::kNoteOnEvent;
                const int key = on ? e.noteOn.pitch : e.noteOff.pitch;
                const float velocity = on ? e.noteOn.velocity : e.noteOff.velocity;
                if (key < 0 || key > 127)
                    continue;
                context.notesOut[made++] = NoteEvent{std::clamp(e.sampleOffset, 0, context.numFrames - 1),
                    on ? NoteEvent::Type::On : NoteEvent::Type::Off, static_cast<std::int16_t>(key),
                    std::isfinite(velocity) ? std::clamp(velocity, 0.0f, 1.0f) : 0.0f,
                    static_cast<std::int16_t>(
                        std::clamp<int>(on ? e.noteOn.channel : e.noteOff.channel, 0, 15))};
            }
            *context.notesOutCount = static_cast<int>(made);
        }
        // Its outputs after the main one, for the host to route apart.
        for (std::size_t k = 0; k + 1 < context.extraOutputs.size(); k += 2) {
            const std::size_t bus = k / 2 + 1;
            if (bus >= outputs_.size() || outputs_[bus].pointers.empty())
                continue; // no such output: the host's silence stays
            const BusBuffers& extra = outputs_[bus];
            std::copy_n(extra.pointers[0], frames, context.extraOutputs[k]);
            std::copy_n(
                extra.pointers[extra.pointers.size() > 1 ? 1 : 0], frames, context.extraOutputs[k + 1]);
        }
        if (outputs_.empty())
            return;

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

    void idle() override { runLoop_.run(); }

    [[nodiscard]] bool hasEditor() const override
    {
        if (!editorChecked_) { // a view is made once to ask; not every plugin has one
            editorChecked_ = true;
            Steinberg::IPlugView* view
                = controller_ != nullptr ? controller_->createView(vst::ViewType::kEditor) : nullptr;
            hasEditor_ = view != nullptr && view->isPlatformTypeSupported(kPlatformType) == kResultOk;
            drop(view);
        }
        return hasEditor_;
    }

    bool openEditor(void* parent, EditorListener* listener) override
    {
        closeEditor();
        if (parent == nullptr || controller_ == nullptr)
            return false;
        view_ = controller_->createView(vst::ViewType::kEditor);
        if (view_ == nullptr || view_->isPlatformTypeSupported(kPlatformType) != kResultOk) {
            drop(view_);
            return false;
        }
        listener_ = listener; // the editor may ask for a size while it attaches
        view_->setFrame(&frame_);
        if (view_->attached(parent, kPlatformType) != kResultOk) {
            view_->setFrame(nullptr);
            drop(view_);
            listener_ = nullptr;
            return false;
        }
        return true;
    }

    void closeEditor() override
    {
        if (view_ == nullptr)
            return;
        view_->removed();
        view_->setFrame(nullptr);
        drop(view_);
        listener_ = nullptr;
    }

    [[nodiscard]] bool isEditorOpen() const override { return view_ != nullptr; }

    [[nodiscard]] EditorSize editorSize() const override
    {
        Steinberg::ViewRect rect;
        if (view_ == nullptr || view_->getSize(&rect) != kResultOk)
            return {};
        return {std::max(rect.getWidth(), 0), std::max(rect.getHeight(), 0)};
    }

    [[nodiscard]] bool canResizeEditor() const override
    {
        return view_ != nullptr && view_->canResize() == kResultOk;
    }

    EditorSize setEditorSize(EditorSize wanted) override
    {
        if (canResizeEditor()) {
            Steinberg::ViewRect rect{0, 0, std::max(wanted.width, 0), std::max(wanted.height, 0)};
            view_->checkSizeConstraint(&rect);
            view_->onSize(&rect);
        }
        return editorSize();
    }

    bool parameterChangedByHost(std::uint32_t id, double value) override
    {
        if (controller_ != nullptr && std::isfinite(value))
            controller_->setParamNormalized(id, std::clamp(value, 0.0, 1.0));
        return false; // the processor still gets it as an event
    }

private:
    /// A parameter edited in the editor, on its way to the audio thread.
    struct Edit {
        vst::ParamID id = 0;
        vst::ParamValue value = 0.0;
    };

    void edited(vst::ParamID id, vst::ParamValue value) override
    {
        edits_.push(Edit{id, value}); // full: dropped; the controller still shows it
        if (listener_ != nullptr)
            listener_->parameterEdited(id, value);
    }

    tresult resizeRequested(Steinberg::IPlugView* view, Steinberg::ViewRect* size) override
    {
        if (view == nullptr || view != view_ || size == nullptr)
            return kInvalidArgument;
        if (listener_ != nullptr)
            listener_->editorResizeRequested({std::max(size->getWidth(), 0), std::max(size->getHeight(), 0)});
        return view_->onSize(size); // the host resized its window: tell the view
    }

    plugin_editor::RunLoop& runLoop() override { return runLoop_; }

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
                // The main bus, the sidechain, and the outputs a host may route apart.
                bool wanted = i == 0;
                if (direction == vst::kOutput) {
                    wanted = i <= static_cast<int32>(PluginInstance::kMaxExtraOutputs);
                } else if (i == 1) {
                    // A second input is a sidechain only if the plugin calls it auxiliary.
                    vst::BusInfo second{};
                    sidechainBus_ = component_->getBusInfo(vst::kAudio, vst::kInput, 1, second) == kResultOk
                        && second.busType == vst::kAux;
                    wanted = sidechainBus_;
                }
                component_->activateBus(vst::kAudio, direction, i, wanted);
            }
            return true;
        };
        if (!size(inCount, vst::kInput, inputs_, inputBuffers_)
            || !size(outCount, vst::kOutput, outputs_, outputBuffers_))
            return false;
        hasSidechain_ = sidechainBus_ && inputs_.size() > 1 && !inputs_[1].pointers.empty();
        extraOutputs_ = 0;
        for (std::size_t bus = 1; bus < outputs_.size() && bus <= PluginInstance::kMaxExtraOutputs; ++bus)
            if (!outputs_[bus].pointers.empty())
                extraOutputs_ = static_cast<int>(bus);
        hasEventOutput_ = component_->getBusCount(vst::kEvent, vst::kOutput) > 0;
        if (hasEventOutput_)
            component_->activateBus(vst::kEvent, vst::kOutput, 0, true);
        hasEventInput_ = component_->getBusCount(vst::kEvent, vst::kInput) > 0;
        if (hasEventInput_)
            component_->activateBus(vst::kEvent, vst::kInput, 0, true);
        return true;
    }

    std::shared_ptr<Vst3Library> library_; ///< kept loaded while the plugin lives
    PluginInfo info_;
    HostApplication host_;
    plugin_editor::RunLoop runLoop_; ///< before the frame, which removes from it
    ComponentHandler handler_{*this};
    PlugFrame frame_{*this};
    Steinberg::IPlugView* view_ = nullptr;
    EditorListener* listener_ = nullptr;
    mutable bool editorChecked_ = false;
    mutable bool hasEditor_ = false;
    SpscQueue<Edit, 256> edits_; ///< control thread to audio thread
    vst::IComponent* component_ = nullptr;
    vst::IAudioProcessor* processor_ = nullptr;
    vst::IEditController* controller_ = nullptr;
    bool separateController_ = false;
    vst::IConnectionPoint* componentConnection_ = nullptr;
    vst::IConnectionPoint* controllerConnection_ = nullptr;

    bool active_ = false;
    bool processing_ = false; ///< audio thread, once active
    bool hasEventInput_ = false;
    bool hasEventOutput_ = false;
    bool hasSidechain_ = false; ///< it has a second audio input bus that is a sidechain
    bool sidechainBus_ = false; ///< configureBuses(): input bus 1 is of the auxiliary kind
    int extraOutputs_ = 0; ///< its output buses after the main one that a host can route
    /// Per MIDI control (0 .. 127 controllers, 128 pressure, 129 pitch
    /// bend): the parameter the plugin moves for it, if any.
    std::array<std::array<bool, 130>, 16> controlMapped_{}; ///< per MIDI channel
    std::array<std::array<vst::ParamID, 130>, 16> controlParameter_{};
    vst::IUnitInfo* units_ = nullptr; ///< where its program names are
    bool hasProgramParameter_ = false;
    vst::ParamID programParameter_ = 0;
    static constexpr int32 kMaxPrograms = 4096;

    /// How many programs its first list has, if it has a way to switch.
    [[nodiscard]] int programCount() const
    {
        vst::ProgramListInfo list{};
        if (units_ == nullptr || !hasProgramParameter_ || units_->getProgramListCount() < 1
            || units_->getProgramListInfo(0, list) != kResultOk)
            return 0;
        return std::clamp<int32>(list.programCount, 0, kMaxPrograms);
    }
    bool resetPending_ = false; ///< audio thread
    double sampleRate_ = 48000.0;
    std::array<std::uint16_t, 128> held_{}; ///< audio thread: per key, the channels it was told it is down on

    std::vector<BusBuffers> inputs_, outputs_;
    std::vector<vst::AudioBusBuffers> inputBuffers_, outputBuffers_;
    ParamChanges changes_;
    EventList events_;
    EventList madeEvents_; ///< the notes the plugin put out this block
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
    /// What the factory is given as the host (IPluginFactory3). First, so it
    /// is still there while the module unloads: a plugin may keep it.
    HostApplication host;
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
        fail(error, "no such file: " + plugin_files::utf8(key));
        return nullptr;
    }
    if (!impl->module.load(binaryOf(key), error))
        return nullptr;
    auto getFactory = functionAt<GetFactory>(impl->module.symbol("GetPluginFactory"));
    if (getFactory == nullptr) {
        fail(error, plugin_files::utf8(key) + " is not a VST 3 plugin (no GetPluginFactory)");
        return nullptr;
    }
    if (!impl->enter()) {
        fail(error, plugin_files::utf8(key) + " failed to initialise");
        return nullptr;
    }
    impl->factory = getFactory();
    if (impl->factory == nullptr) {
        fail(error, plugin_files::utf8(key) + " has no plugin factory");
        return nullptr;
    }
    // Before the classes are read: a shell (WaveShell) lists its edit
    // controllers only once it knows the host, and its plugins cannot be
    // created without them.
    Steinberg::IPluginFactory3* factory3 = nullptr;
    if (impl->factory->queryInterface(
            Steinberg::IPluginFactory3::iid.toTUID(), reinterpret_cast<void**>(&factory3))
            == kResultOk
        && factory3 != nullptr) {
        factory3->setHostContext(&impl->host);
        factory3->release();
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
        fail(error, "no plugin " + std::string(id) + " in " + plugin_files::utf8(impl_->path));
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
