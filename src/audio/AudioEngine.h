#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "ChainProcessor.h"
#include "LevelMeterSource.h"

#include <atomic>

// Opens the audio device and runs: selected input channel -> stereo (L = R)
// -> ChainProcessor -> output channels 0 and 1 (further outputs are silent;
// with one output channel only L is written).
//
// Threads: initialise/shutdown/prepareForTest/setInputChannel/getChain().post
// run on the message thread. Lifecycle callbacks may also come from a driver
// thread; JUCE excludes IO during them, and ChainProcessor serialises them
// with message-thread edits. The IO callback never allocates, frees, locks or logs.
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    // Opens the default device (1 input, 2 outputs). Returns an empty string on
    // success, otherwise a Korean error message. The engine is registered as
    // the device manager's callback either way, so a device opened later (for
    // example from the audio settings dialog) is processed too.
    juce::String initialise();
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    ChainProcessor& getChain() { return chain; }

    // Same preparation/start as audioDeviceAboutToStart, without a device.
    // Only with no real callback registered and no simulated IO in flight.
    void prepareForTest (double sampleRate, int blockSize);

    void setInputChannel (int index) { inputChannel.store (index); }

    LevelMeterSource& getInputMeter() { return inputMeter; }
    LevelMeterSource& getOutputMeter() { return outputMeter; }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    juce::AudioDeviceManager deviceManager;
    ChainProcessor chain;
    juce::AudioBuffer<float> workBuffer;   // 2 channels x prepared block size
    int preparedBlockSize = 0;
    std::atomic<int> inputChannel { 0 };
    LevelMeterSource inputMeter, outputMeter;

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
