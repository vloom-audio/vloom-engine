// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// macOS MIDI input through CoreMIDI. The port is created for MIDI 1.0
// (macOS 11 and later): CoreMIDI hands over Universal MIDI Packets whose
// channel-voice words hold the classic bytes, on a thread of its own.
#include "io/MidiBackend.hpp"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

namespace vloom::io {

namespace {

std::string toUtf8(CFStringRef text)
{
    if (text == nullptr)
        return {};
    const CFIndex length = CFStringGetLength(text);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8))
        return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

std::string sourceName(MIDIEndpointRef source)
{
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(source, kMIDIPropertyDisplayName, &name) != noErr || name == nullptr)
        return {};
    std::string out = toUtf8(name);
    CFRelease(name);
    return out;
}

/// Words in a Universal MIDI Packet message, by its type (the top 4 bits).
int umpWords(std::uint32_t word) noexcept
{
    static constexpr int kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return kWords[(word >> 28) & 0xF];
}

/// Passes on the MIDI 1.0 channel-voice messages (type 2) of a list.
void readList(const MIDIEventList* list, const MidiDeliver& deliver)
{
    if (list == nullptr)
        return;
    const MIDIEventPacket* packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
        for (UInt32 w = 0; w < packet->wordCount && w < 64;) {
            const std::uint32_t word = packet->words[w];
            if (((word >> 28) & 0xF) == 0x2) {
                MidiMessage message;
                message.status = static_cast<std::uint8_t>((word >> 16) & 0xFF);
                message.data1 = static_cast<std::uint8_t>((word >> 8) & 0x7F);
                message.data2 = static_cast<std::uint8_t>(word & 0x7F);
                if (message.status >= 0x80 && message.status < 0xF0)
                    deliver(message);
            }
            w += static_cast<UInt32>(umpWords(word));
        }
        packet = MIDIEventPacketNext(packet);
    }
}

class CoreMidiInput final : public MidiBackend {
public:
    explicit CoreMidiInput(MidiDeliver deliver)
        : deliver_(std::make_shared<MidiDeliver>(std::move(deliver)))
    {
    }
    ~CoreMidiInput() override { close(); }
    CoreMidiInput(const CoreMidiInput&) = delete;
    CoreMidiInput& operator=(const CoreMidiInput&) = delete;

    bool open(int index, std::string* error) override
    {
        close();
        const auto fail = [&](const char* what) {
            if (error != nullptr)
                *error = what;
            close();
            return false;
        };
        if (index < 0 || static_cast<ItemCount>(index) >= MIDIGetNumberOfSources())
            return fail("no MIDI input with that number");
        source_ = MIDIGetSource(static_cast<ItemCount>(index));
        if (source_ == 0)
            return fail("the MIDI input has gone");
        if (__builtin_available(macOS 11.0, *)) {
            if (MIDIClientCreate(CFSTR("Vloom"), nullptr, nullptr, &client_) != noErr)
                return fail("cannot reach CoreMIDI");
            // The block owns its own reference to the callback: CoreMIDI may
            // still be in it while the port is disposed of.
            std::shared_ptr<MidiDeliver> deliver = deliver_;
            const OSStatus status = MIDIInputPortCreateWithProtocol(client_, CFSTR("Vloom input"),
                kMIDIProtocol_1_0, &port_, ^(const MIDIEventList* list, void*) { readList(list, *deliver); });
            if (status != noErr)
                return fail("cannot create a CoreMIDI input port");
        } else {
            return fail("MIDI input needs macOS 11 or later");
        }
        name_ = sourceName(source_);
        return true;
    }

    bool start(std::string* error) override
    {
        if (port_ == 0)
            return false;
        if (connected_)
            return true;
        if (MIDIPortConnectSource(port_, source_, nullptr) != noErr) {
            if (error != nullptr)
                *error = "cannot connect to the MIDI input";
            return false;
        }
        connected_ = true;
        return true;
    }

    void stop() override
    {
        if (connected_ && port_ != 0)
            MIDIPortDisconnectSource(port_, source_);
        connected_ = false;
    }

    void close() override
    {
        stop();
        if (port_ != 0)
            MIDIPortDispose(port_);
        if (client_ != 0)
            MIDIClientDispose(client_);
        port_ = 0;
        client_ = 0;
        source_ = 0;
        name_.clear();
    }

    [[nodiscard]] std::string name() const override { return name_; }

private:
    std::shared_ptr<MidiDeliver> deliver_;
    MIDIClientRef client_ = 0;
    MIDIPortRef port_ = 0;
    MIDIEndpointRef source_ = 0;
    bool connected_ = false;
    std::string name_;
};

} // namespace

std::vector<std::string> midiInputNames()
{
    std::vector<std::string> names;
    const ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i)
        names.push_back(sourceName(MIDIGetSource(i)));
    return names;
}

std::unique_ptr<MidiBackend> makeMidiBackend(MidiDeliver deliver)
{
    return std::make_unique<CoreMidiInput>(std::move(deliver));
}

} // namespace vloom::io

#endif // __APPLE__
