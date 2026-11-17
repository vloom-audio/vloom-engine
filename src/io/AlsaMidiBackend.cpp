// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Linux MIDI input through the ALSA sequencer: USB keyboards, virtual ports
// of other programs, and what PipeWire or JACK bridge to it. Our own client
// subscribes one port of ours to the chosen source; a thread of ours reads
// its events and turns them back into MIDI bytes.
#include "io/MidiBackend.hpp"

#if defined(__linux__)

#include <algorithm>
#include <alsa/asoundlib.h>
#include <atomic>
#include <poll.h>
#include <thread>

namespace vloom::io {

namespace {

struct Address {
    int client = 0;
    int port = 0;
    std::string name;
};

/// The ports another client lets us read from (system ports left out).
std::vector<Address> readablePorts(snd_seq_t* seq)
{
    std::vector<Address> ports;
    snd_seq_client_info_t* client = nullptr;
    snd_seq_port_info_t* port = nullptr;
    snd_seq_client_info_malloc(&client);
    snd_seq_port_info_malloc(&port);
    if (client == nullptr || port == nullptr) {
        snd_seq_client_info_free(client);
        snd_seq_port_info_free(port);
        return ports;
    }
    const int self = snd_seq_client_id(seq);
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) >= 0) {
        const int id = snd_seq_client_info_get_client(client);
        if (id == SND_SEQ_CLIENT_SYSTEM || id == self)
            continue;
        snd_seq_port_info_set_client(port, id);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq, port) >= 0) {
            constexpr unsigned kReadable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((snd_seq_port_info_get_capability(port) & kReadable) != kReadable
                || (snd_seq_port_info_get_capability(port) & SND_SEQ_PORT_CAP_NO_EXPORT) != 0)
                continue;
            const char* clientName = snd_seq_client_info_get_name(client);
            const char* portName = snd_seq_port_info_get_name(port);
            std::string name = clientName != nullptr ? clientName : "";
            if (portName != nullptr && name != portName)
                name += std::string(name.empty() ? "" : ": ") + portName;
            ports.push_back(Address{id, snd_seq_port_info_get_port(port), std::move(name)});
        }
    }
    snd_seq_client_info_free(client);
    snd_seq_port_info_free(port);
    return ports;
}

/// An event read back as the bytes it came from; status 0 for the others.
MidiMessage toMessage(const snd_seq_event_t& event) noexcept
{
    MidiMessage message;
    const auto channel = static_cast<std::uint8_t>(event.data.control.channel & 0x0F);
    const auto byte = [](int value) { return static_cast<std::uint8_t>(value & 0x7F); };
    switch (event.type) {
    case SND_SEQ_EVENT_NOTEON:
        return MidiMessage{static_cast<std::uint8_t>(0x90 | (event.data.note.channel & 0x0F)),
            byte(event.data.note.note), byte(event.data.note.velocity)};
    case SND_SEQ_EVENT_NOTEOFF:
        return MidiMessage{static_cast<std::uint8_t>(0x80 | (event.data.note.channel & 0x0F)),
            byte(event.data.note.note), byte(event.data.note.velocity)};
    case SND_SEQ_EVENT_KEYPRESS:
        return MidiMessage{static_cast<std::uint8_t>(0xA0 | (event.data.note.channel & 0x0F)),
            byte(event.data.note.note), byte(event.data.note.velocity)};
    case SND_SEQ_EVENT_CONTROLLER:
        return MidiMessage{static_cast<std::uint8_t>(0xB0 | channel),
            byte(static_cast<int>(event.data.control.param)), byte(event.data.control.value)};
    case SND_SEQ_EVENT_PGMCHANGE:
        return MidiMessage{static_cast<std::uint8_t>(0xC0 | channel), byte(event.data.control.value), 0};
    case SND_SEQ_EVENT_CHANPRESS:
        return MidiMessage{static_cast<std::uint8_t>(0xD0 | channel), byte(event.data.control.value), 0};
    case SND_SEQ_EVENT_PITCHBEND: {
        const int bend = std::clamp(event.data.control.value + 8192, 0, 16383);
        return MidiMessage{static_cast<std::uint8_t>(0xE0 | channel), byte(bend), byte(bend >> 7)};
    }
    default:
        return message;
    }
}

class AlsaMidiInput final : public MidiBackend {
public:
    explicit AlsaMidiInput(MidiDeliver deliver)
        : deliver_(std::move(deliver))
    {
    }
    ~AlsaMidiInput() override { close(); }
    AlsaMidiInput(const AlsaMidiInput&) = delete;
    AlsaMidiInput& operator=(const AlsaMidiInput&) = delete;

    bool open(int index, std::string* error) override
    {
        close();
        const auto fail = [&](const char* what) {
            if (error != nullptr)
                *error = what;
            close();
            return false;
        };
        if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
            seq_ = nullptr;
            return fail("cannot open the ALSA sequencer");
        }
        snd_seq_set_client_name(seq_, "Vloom");
        const std::vector<Address> ports = readablePorts(seq_);
        if (index < 0 || static_cast<std::size_t>(index) >= ports.size())
            return fail("no MIDI input with that number");
        source_ = ports[static_cast<std::size_t>(index)];
        port_ = snd_seq_create_simple_port(seq_, "Vloom input",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (port_ < 0)
            return fail("cannot create an ALSA sequencer port");
        return true;
    }

    bool start(std::string* error) override
    {
        if (seq_ == nullptr)
            return false;
        if (reader_.joinable())
            return true;
        if (snd_seq_connect_from(seq_, port_, source_.client, source_.port) < 0) {
            if (error != nullptr)
                *error = "cannot connect to the MIDI input";
            return false;
        }
        quit_.store(false);
        reader_ = std::thread([this] { read(); });
        return true;
    }

    void stop() override
    {
        if (!reader_.joinable())
            return;
        quit_.store(true);
        reader_.join(); // wakes within its poll timeout
        snd_seq_disconnect_from(seq_, port_, source_.client, source_.port);
    }

    void close() override
    {
        stop();
        if (seq_ != nullptr)
            snd_seq_close(seq_);
        seq_ = nullptr;
        port_ = -1;
        source_ = {};
    }

    [[nodiscard]] std::string name() const override { return source_.name; }

private:
    void read()
    {
        const int count = snd_seq_poll_descriptors_count(seq_, POLLIN);
        std::vector<pollfd> fds(static_cast<std::size_t>(count > 0 ? count : 0));
        if (!fds.empty())
            snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(fds.size()), POLLIN);
        while (!quit_.load()) {
            if (!fds.empty() && poll(fds.data(), static_cast<nfds_t>(fds.size()), 50) <= 0)
                continue;
            for (;;) {
                snd_seq_event_t* event = nullptr;
                if (snd_seq_event_input(seq_, &event) < 0 || event == nullptr)
                    break; // -EAGAIN: nothing more for now
                const MidiMessage message = toMessage(*event);
                if (message.status != 0) {
                    try {
                        deliver_(message);
                    } catch (...) {
                    }
                }
            }
        }
    }

    MidiDeliver deliver_;
    snd_seq_t* seq_ = nullptr;
    int port_ = -1;
    Address source_;
    std::atomic<bool> quit_{false};
    std::thread reader_;
};

} // namespace

std::vector<std::string> midiInputNames()
{
    std::vector<std::string> names;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        return names; // no sequencer (a container, a server without sound)
    for (Address& port : readablePorts(seq))
        names.push_back(std::move(port.name));
    snd_seq_close(seq);
    return names;
}

std::unique_ptr<MidiBackend> makeMidiBackend(MidiDeliver deliver)
{
    return std::make_unique<AlsaMidiInput>(std::move(deliver));
}

} // namespace vloom::io

#endif // __linux__
