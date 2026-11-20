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
// run on the message thread, including aboutToStart/prepare. Only stop/error
// callbacks may come from a driver thread, with IO excluded by JUCE.
// The IO callback never allocates, frees, locks or logs.
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    // Opens the device (1 input, 2 outputs): the one described by
    // savedDeviceState (AudioDeviceManager::createStateXml() from an earlier
    // run: device type such as ASIO, devices, rate, buffer, channels), falling
    // back to the default device when it is null or that device is gone.
    // Returns an empty string on success, otherwise a Korean error message.
    // The engine is registered as the device manager's callback either way, so
    // a device opened later (for example from the audio settings dialog) is
    // processed too.
    juce::String initialise (const juce::XmlElement* savedDeviceState = nullptr);
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    ChainProcessor& getChain() { return chain; }

    // Same preparation/start as audioDeviceAboutToStart, without a device.
    // Only with no real callback registered and no simulated IO in flight.
    void prepareForTest (double sampleRate, int blockSize);

    void setInputChannel (int index) { inputChannel.store (index); }
    void setOutputMuted (bool muted) { outputMuted.store (muted); }
    bool isOutputMuted() const { return outputMuted.load(); }

    // Mono output: after the chain (and before mute) left and right are both
    // set to (L + R) / 2, so stereo effects cannot leave the voice lopsided.
    // Switching fades over kOutputDipSeconds (no click); a value set before
    // the device starts applies from the first sample. Any thread.
    void setOutputMono (bool mono) { outputMono.store (mono); }
    bool isOutputMono() const { return outputMono.load(); }

    LevelMeterSource& getInputMeter() { return inputMeter; }
    LevelMeterSource& getOutputMeter() { return outputMeter; }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;

private:
    juce::AudioDeviceManager deviceManager;
    ChainProcessor chain;
    juce::AudioBuffer<float> workBuffer;   // 2 channels x prepared block size
    std::atomic<int> preparedBlockSize { 0 };
    std::atomic<int> inputChannel { 0 };
    std::atomic<bool> outputMuted { false };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> muteGain { 1.0f };
    std::atomic<bool> outputMono { false };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> monoMix { 0.0f };   // 0 = stereo, 1 = mono
    LevelMeterSource inputMeter, outputMeter;

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
