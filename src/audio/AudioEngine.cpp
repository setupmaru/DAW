#include "AudioEngine.h"

#include <algorithm>

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise()
{
    auto error = deviceManager.initialiseWithDefaultDevices (1, 2);

    // Registered even on failure: a device the user picks later in the audio
    // settings dialog then runs through this engine.
    deviceManager.addAudioCallback (this);

    if (error.isNotEmpty())
        return juce::String (juce::CharPointer_UTF8 ("오디오 장치를 열 수 없습니다: ")) + error;

    return {};
}

void AudioEngine::shutdown()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void AudioEngine::prepareForTest (double sampleRate, int blockSize)
{
    preparedBlockSize = std::max (1, blockSize);
    workBuffer.setSize (2, preparedBlockSize);
    workBuffer.clear();
    chain.prepare (sampleRate, preparedBlockSize);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
        prepareForTest (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped() {}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Flush denormals to zero for the whole callback (no allocation).
    juce::ScopedNoDenormals noDenormals;

    // Not prepared: output silence.
    if (preparedBlockSize <= 0)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

        return;
    }

    const int selected = inputChannel.load();
    const float* source = (selected >= 0 && selected < numInputChannels) ? inputChannelData[selected] : nullptr;

    float* work[2] = { workBuffer.getWritePointer (0), workBuffer.getWritePointer (1) };

    for (int start = 0; start < numSamples; start += preparedBlockSize)
    {
        const int n = std::min (preparedBlockSize, numSamples - start);

        if (source != nullptr)
            juce::FloatVectorOperations::copy (work[0], source + start, n);
        else
            juce::FloatVectorOperations::clear (work[0], n);

        juce::FloatVectorOperations::copy (work[1], work[0], n);

        // A view onto the work buffer: no allocation.
        juce::AudioBuffer<float> view (work, 2, n);

        inputMeter.pushPeak (view.getMagnitude (0, 0, n));
        chain.process (view);
        outputMeter.pushPeak (std::max (view.getMagnitude (0, 0, n), view.getMagnitude (1, 0, n)));

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] == nullptr)
                continue;

            if (ch < 2)
                juce::FloatVectorOperations::copy (outputChannelData[ch] + start, work[ch], n);
            else
                juce::FloatVectorOperations::clear (outputChannelData[ch] + start, n);
        }
    }
}
