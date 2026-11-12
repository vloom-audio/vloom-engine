// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// macOS output through a HAL output Audio Unit (AUHAL). The unit converts
// our float stream to the device's own rate and format, and calls the
// render callback on CoreAudio's real-time I/O thread.
#include "io/AudioBackend.hpp"

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <atomic>
#include <string>

namespace vloom::io {

namespace {

AudioObjectPropertyAddress address(
    AudioObjectPropertySelector selector, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

AudioDeviceID defaultOutputDevice()
{
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const auto where = address(kAudioHardwarePropertyDefaultOutputDevice);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, &size, &device) != noErr)
        return kAudioObjectUnknown;
    return device;
}

std::string deviceName(AudioDeviceID device)
{
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    const auto where = address(kAudioObjectPropertyName);
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, &name) != noErr || name == nullptr)
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

int outputChannels(AudioDeviceID device)
{
    const auto where = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    // AudioBufferList has a flexible array: read it into a raw buffer.
    std::string storage(size, '\0');
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, list) != noErr)
        return 0;
    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return channels;
}

class CoreAudioBackend final : public AudioBackend {
public:
    ~CoreAudioBackend() override { close(); }

    std::vector<BackendDeviceInfo> listOutputDevices() override
    {
        std::vector<BackendDeviceInfo> result;
        const auto where = address(kAudioHardwarePropertyDevices);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &where, 0, nullptr, &size) != noErr)
            return result;
        std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
        if (devices.empty()
            || AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, &size, devices.data())
                != noErr)
            return result;
        const AudioDeviceID fallback = defaultOutputDevice();
        for (const AudioDeviceID device : devices) {
            if (outputChannels(device) <= 0)
                continue; // input-only
            result.push_back({std::to_string(device), deviceName(device), device == fallback});
        }
        return result;
    }

    bool open(const BackendStreamConfig& config, RenderFn render, void* user, BackendStreamInfo& info,
        std::string& error) override
    {
        close();
        AudioDeviceID device = defaultOutputDevice();
        if (!config.deviceId.empty()) {
            try {
                device = static_cast<AudioDeviceID>(std::stoul(config.deviceId));
            } catch (...) {
                device = kAudioObjectUnknown;
            }
        }
        if (device == kAudioObjectUnknown) {
            error = "No audio output device";
            return false;
        }

        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_HALOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr || AudioComponentInstanceNew(component, &unit_) != noErr) {
            unit_ = nullptr;
            error = "Could not create the audio output unit";
            return false;
        }

        if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                &device, sizeof(device))
            != noErr) {
            close();
            error = "The audio device is not available";
            return false;
        }

        double sampleRate = config.sampleRate;
        if (sampleRate <= 0.0) {
            Float64 nominal = 48000.0;
            UInt32 size = sizeof(nominal);
            const auto where = address(kAudioDevicePropertyNominalSampleRate);
            AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, &nominal);
            sampleRate = nominal;
        }
        const int channels = std::clamp(config.channels, 1, 8);

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
                unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format))
            != noErr) {
            close();
            error = "The audio device does not accept this format";
            return false;
        }

        // Ask the device for the requested period; it may pick another one.
        UInt32 frames = static_cast<UInt32>(config.bufferFrames);
        const auto bufferSize = address(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(device, &bufferSize, 0, nullptr, sizeof(frames), &frames);

        render_ = render;
        user_ = user;
        channels_ = channels;
        AURenderCallbackStruct callback{&CoreAudioBackend::renderCallback, this};
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                &callback, sizeof(callback))
                != noErr
            || AudioUnitInitialize(unit_) != noErr) {
            close();
            error = "Could not open the audio device";
            return false;
        }

        info.sampleRate = static_cast<int>(sampleRate);
        info.channels = channels;
        info.deviceName = deviceName(device);
        return true;
    }

    bool start(std::string& error) override
    {
        if (unit_ == nullptr) {
            error = "Audio device is not open";
            return false;
        }
        if (AudioOutputUnitStart(unit_) != noErr) {
            error = "Could not start the audio device";
            return false;
        }
        running_.store(true);
        return true;
    }

    void stop() noexcept override
    {
        // AudioOutputUnitStop waits for a running render callback to return.
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
    // CoreAudio's real-time I/O thread.
    static OSStatus renderCallback(void* self, AudioUnitRenderActionFlags* /*flags*/,
        const AudioTimeStamp* /*time*/, UInt32 /*bus*/, UInt32 frames, AudioBufferList* buffers)
    {
        auto* backend = static_cast<CoreAudioBackend*>(self);
        if (buffers == nullptr || buffers->mNumberBuffers < 1 || buffers->mBuffers[0].mData == nullptr)
            return noErr;
        backend->render_(backend->user_, static_cast<float*>(buffers->mBuffers[0].mData), backend->channels_,
            static_cast<int>(frames));
        return noErr;
    }

    AudioComponentInstance unit_ = nullptr;
    RenderFn render_ = nullptr;
    void* user_ = nullptr;
    int channels_ = 2;
    std::atomic<bool> running_{false};
};

} // namespace

std::unique_ptr<AudioBackend> createPlatformBackend() { return std::make_unique<CoreAudioBackend>(); }

} // namespace vloom::io

#endif // __APPLE__
