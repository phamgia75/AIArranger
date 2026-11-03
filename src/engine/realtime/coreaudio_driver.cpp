#include "engine/realtime/coreaudio_driver.h"
#include <cstdio>
#include <mach/mach_time.h>

namespace ai_arranger::realtime {

CoreAudioDriver::CoreAudioDriver() {
    clock_.setSampleRate(48000);
    clock_.setTempo(120);
}

CoreAudioDriver::~CoreAudioDriver() {
    stop();
    if (audio_unit_) {
        AudioUnitUninitialize(audio_unit_);
        audio_unit_ = nullptr;
    }
}

bool CoreAudioDriver::init(uint32_t sampleRate) noexcept {
    // Find default output AudioUnit
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    component_ = AudioComponentFindNext(nullptr, &desc);
    if (!component_) {
        std::fprintf(stderr, "[CoreAudio] Failed to find output AudioUnit\n");
        return false;
    }

    OSStatus status = AudioComponentInstanceNew(component_, &audio_unit_);
    if (status != noErr || !audio_unit_) {
        std::fprintf(stderr, "[CoreAudio] Failed to create AudioUnit instance: %d\n", status);
        return false;
    }

    // Set stream format
    AudioStreamBasicDescription streamFormat{};
    streamFormat.mSampleRate = sampleRate;
    streamFormat.mFormatID = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    streamFormat.mFramesPerPacket = 1;
    streamFormat.mChannelsPerFrame = 2;
    streamFormat.mBytesPerFrame = sizeof(float) * 2;
    streamFormat.mBytesPerPacket = streamFormat.mBytesPerFrame;
    streamFormat.mBitsPerChannel = sizeof(float) * 8;

    status = AudioUnitSetProperty(audio_unit_,
                                   kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input,
                                   0,
                                   &streamFormat,
                                   sizeof(streamFormat));
    if (status != noErr) {
        std::fprintf(stderr, "[CoreAudio] Failed to set stream format: %d\n", status);
        return false;
    }

    // Set render callback
    AURenderCallbackStruct callbackStruct{};
    callbackStruct.inputProc = renderCallback;
    callbackStruct.inputProcRefCon = this;

    status = AudioUnitSetProperty(audio_unit_,
                                   kAudioUnitProperty_SetRenderCallback,
                                   kAudioUnitScope_Input,
                                   0,
                                   &callbackStruct,
                                   sizeof(callbackStruct));
    if (status != noErr) {
        std::fprintf(stderr, "[CoreAudio] Failed to set render callback: %d\n", status);
        return false;
    }

    // Initialize
    status = AudioUnitInitialize(audio_unit_);
    if (status != noErr) {
        std::fprintf(stderr, "[CoreAudio] Failed to initialize: %d\n", status);
        return false;
    }

    clock_.setSampleRate(sampleRate);
    std::printf("[CoreAudio] Initialized at %u Hz\n", sampleRate);
    return true;
}

bool CoreAudioDriver::start() noexcept {
    if (!audio_unit_) return false;

    clock_.reset();
    clock_.start();
    running_.store(true, std::memory_order_release);
    total_samples_.store(0, std::memory_order_release);

    OSStatus status = AudioOutputUnitStart(audio_unit_);
    if (status != noErr) {
        std::fprintf(stderr, "[CoreAudio] Failed to start: %d\n", status);
        running_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void CoreAudioDriver::stop() noexcept {
    if (!audio_unit_) return;
    running_.store(false, std::memory_order_release);
    AudioOutputUnitStop(audio_unit_);
    clock_.stop();
}

// ── Static Render Callback ─────────────────────────────────────────

OSStatus CoreAudioDriver::renderCallback(void* inRefCon,
                                          AudioUnitRenderActionFlags* ioActionFlags,
                                          const AudioTimeStamp* inTimeStamp,
                                          UInt32 inBusNumber,
                                          UInt32 inNumFrames,
                                          AudioBufferList* ioData) {
    auto* driver = static_cast<CoreAudioDriver*>(inRefCon);
    if (!driver) return kAudioUnitErr_InvalidParameter;
    return driver->onRender(ioActionFlags, inTimeStamp, inBusNumber, inNumFrames, ioData);
}

OSStatus CoreAudioDriver::onRender(AudioUnitRenderActionFlags* ioActionFlags,
                                    const AudioTimeStamp* inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumFrames,
                                    AudioBufferList* ioData) {
    if (!running_.load(std::memory_order_acquire)) {
        // Silent output (zero buffers)
        if (ioData) {
            for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i) {
                auto& buf = ioData->mBuffers[i];
                if (buf.mData) __builtin_memset(buf.mData, 0, buf.mDataByteSize);
            }
        }
        return noErr;
    }

    // Get mach_absolute_time for this callback
    uint64_t machTime = mach_absolute_time();
    clock_.setMachTime(machTime);

    // Accumulate sample count
    total_samples_.fetch_add(inNumFrames, std::memory_order_acq_rel);

    // Advance clock
    clock_.advance(inNumFrames);

    // Dispatch pending MIDI events
    scheduler_.advanceTo(clock_.getPosition());

    // Zero output buffers (no sound generation yet)
    if (ioData) {
        for (UInt32 i = 0; i < ioData->mNumberBuffers; ++i) {
            auto& buf = ioData->mBuffers[i];
            if (buf.mData) __builtin_memset(buf.mData, 0, buf.mDataByteSize);
        }
    }

    return noErr;
}

} // namespace ai_arranger::realtime
