// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// macOS input through CoreAudio: a HAL output unit with only its input
// side enabled (bus 1), pulled with AudioUnitRender() from its input
// callback on CoreAudio's real-time thread. The HAL unit does not convert
// the input's sample rate, so the device is asked to run at the rate wanted.
#include "io/InputBackend.hpp"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace vloom::io {

namespace {

AudioObjectPropertyAddress where(
    AudioObjectPropertySelector selector, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

AudioDeviceID defaultInputDevice()
{
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const auto address = where(kAudioHardwarePropertyDefaultInputDevice);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr)
        return kAudioObjectUnknown;
    return device;
}

std::string nameOf(AudioDeviceID device)
{
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    const auto address = where(kAudioObjectPropertyName);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) != noErr || name == nullptr)
        return {};
    const CFIndex length = CFStringGetLength(name);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(capacity), '\0');
    if (CFStringGetCString(name, out.data(), capacity, kCFStringEncodingUTF8))
        out.resize(std::char_traits<char>::length(out.c_str()));
    else
        out.clear();
    CFRelease(name);
    return out;
}

int inputChannels(AudioDeviceID device)
{
    const auto address = where(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    std::string storage(size, '\0'); // AudioBufferList has a flexible array
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr)
        return 0;
    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return channels;
}

double nominalRate(AudioDeviceID device)
{
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    const auto address = where(kAudioDevicePropertyNominalSampleRate);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) != noErr)
        return 0.0;
    return rate;
}

class CoreAudioInputBackend final : public InputBackend {
public:
    ~CoreAudioInputBackend() override { close(); }

    std::vector<BackendDeviceInfo> listInputDevices() override
    {
        std::vector<BackendDeviceInfo> result;
        const auto address = where(kAudioHardwarePropertyDevices);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
            return result;
        std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
        if (devices.empty()
            || AudioObjectGetPropertyData(
                   kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data())
                != noErr)
            return result;
        const AudioDeviceID fallback = defaultInputDevice();
        for (const AudioDeviceID device : devices)
            if (inputChannels(device) > 0)
                result.push_back({std::to_string(device), nameOf(device), device == fallback});
        return result;
    }

    bool open(const BackendStreamConfig& config, CaptureFn capture, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        AudioDeviceID device = defaultInputDevice();
        if (!config.deviceId.empty()) {
            try {
                device = static_cast<AudioDeviceID>(std::stoul(config.deviceId));
            } catch (...) {
                device = kAudioObjectUnknown;
            }
        }
        if (device == kAudioObjectUnknown || inputChannels(device) <= 0) {
            error = "No audio input device";
            return false;
        }
        const auto fail = [&](const char* message) {
            close();
            error = message;
            return false;
        };

        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_HALOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr || AudioComponentInstanceNew(component, &unit_) != noErr) {
            unit_ = nullptr;
            return fail("Could not create the audio input unit");
        }
        // Input on (bus 1), output off (bus 0): before the device is set.
        UInt32 on = 1;
        UInt32 off = 0;
        if (AudioUnitSetProperty(
                unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof(on))
                != noErr
            || AudioUnitSetProperty(
                   unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof(off))
                != noErr)
            return fail("Could not set up the audio input unit");
        if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                &device, sizeof(device))
            != noErr)
            return fail(
                "The audio input is not available (is the microphone allowed in the privacy settings?)");

        // The HAL unit does not convert the input's rate: ask the device for it.
        if (config.sampleRate > 0 && nominalRate(device) != static_cast<double>(config.sampleRate)) {
            Float64 wanted = config.sampleRate;
            const auto address = where(kAudioDevicePropertyNominalSampleRate);
            AudioObjectSetPropertyData(device, &address, 0, nullptr, sizeof(wanted), &wanted);
        }
        const double sampleRate = nominalRate(device) > 0.0 ? nominalRate(device) : 48000.0;
        const int channels = std::clamp(std::min(config.channels, inputChannels(device)), 1, 8);

        AudioStreamBasicDescription format{};
        format.mSampleRate = sampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mBytesPerPacket = static_cast<UInt32>(channels) * 4;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = static_cast<UInt32>(channels) * 4;
        format.mChannelsPerFrame = static_cast<UInt32>(channels);
        format.mBitsPerChannel = 32;
        if (AudioUnitSetProperty(
                unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, sizeof(format))
            != noErr)
            return fail("The audio input does not accept this format");

        UInt32 frames = static_cast<UInt32>(config.bufferFrames);
        const auto bufferSize = where(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(device, &bufferSize, 0, nullptr, sizeof(frames), &frames);

        // Room for the largest slice the unit may pull at once.
        UInt32 maxFrames = 4096;
        UInt32 size = sizeof(maxFrames);
        AudioUnitGetProperty(
            unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, &size);
        maxFrames_ = std::max<UInt32>(maxFrames, 4096);
        buffer_.assign(static_cast<std::size_t>(maxFrames_) * static_cast<std::size_t>(channels), 0.0f);

        capture_ = capture;
        user_ = user;
        channels_ = channels;
        AURenderCallbackStruct callback{&CoreAudioInputBackend::inputCallback, this};
        if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                &callback, sizeof(callback))
                != noErr
            || AudioUnitInitialize(unit_) != noErr)
            return fail("Could not open the audio input");

        info.sampleRate = static_cast<int>(sampleRate);
        info.channels = channels;
        info.deviceName = nameOf(device);
        return true;
    }

    bool start(std::string& error) override
    {
        if (unit_ == nullptr) {
            error = "Audio input is not open";
            return false;
        }
        if (!running_.load()) {
            if (AudioOutputUnitStart(unit_) != noErr) {
                error = "Could not start the audio input";
                return false;
            }
            running_.store(true);
        }
        return true;
    }

    void stop() noexcept override
    {
        // AudioOutputUnitStop waits for a running callback to return.
        if (unit_ != nullptr && running_.load())
            AudioOutputUnitStop(unit_);
        running_.store(false);
    }

    void close() noexcept override
    {
        stop();
        if (unit_ != nullptr) {
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }

    [[nodiscard]] bool isRunning() const noexcept override { return running_.load(); }

private:
    // CoreAudio's real-time I/O thread: the input has `frames` ready.
    static OSStatus inputCallback(void* self, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
        UInt32 bus, UInt32 frames, AudioBufferList* /*unused*/)
    {
        auto* backend = static_cast<CoreAudioInputBackend*>(self);
        if (frames == 0 || frames > backend->maxFrames_)
            return noErr;
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = static_cast<UInt32>(backend->channels_);
        list.mBuffers[0].mDataByteSize = frames * static_cast<UInt32>(backend->channels_) * 4;
        list.mBuffers[0].mData = backend->buffer_.data();
        if (AudioUnitRender(backend->unit_, flags, time, bus, frames, &list) != noErr)
            return noErr;
        backend->capture_(
            backend->user_, backend->buffer_.data(), backend->channels_, static_cast<int>(frames));
        return noErr;
    }

    AudioComponentInstance unit_ = nullptr;
    CaptureFn capture_ = nullptr;
    void* user_ = nullptr;
    int channels_ = 2;
    UInt32 maxFrames_ = 4096;
    std::vector<float> buffer_; ///< allocated in open(), filled on the I/O thread
    std::atomic<bool> running_{false};
};

} // namespace

std::unique_ptr<InputBackend> createPlatformInputBackend()
{
    return std::make_unique<CoreAudioInputBackend>();
}

} // namespace vloom::io

#endif // __APPLE__
