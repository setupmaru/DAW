#include "AudioEngine.h"

#include <algorithm>

AudioEngine::AudioEngine()
{
    chain.setProcessingActive (false);
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    auto error = deviceManager.initialise (1, 2, savedDeviceState, true);

    // JUCE's own fallback (selectDefaultDeviceOnFailure) stays inside the saved
    // device type. For ASIO that means another installed ASIO driver, or none
    // at all when the unplugged interface's driver is still registered. Then
    // open the default device of the first other type that has devices (on
    // Windows: Windows Audio). treatAsChosenDevice is false, so the saved
    // choice is kept and the interface opens again once it is back.
    if (savedDeviceState != nullptr)
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        const auto savedOutput = savedDeviceState->getStringAttribute ("audioOutputDeviceName");
        const bool wrongAsioDriver = device != nullptr
                                     && deviceManager.getCurrentAudioDeviceType() == "ASIO"
                                     && savedOutput.isNotEmpty() && device->getName() != savedOutput;

        if (device == nullptr || wrongAsioDriver)
        {
            const auto currentType = deviceManager.getCurrentAudioDeviceType();

            for (auto* type : deviceManager.getAvailableDeviceTypes())
            {
                if (type->getTypeName() == currentType || type->getTypeName() == "ASIO")
                    continue;

                type->scanForDevices();

                if (type->getDeviceNames (false).isEmpty())
                    continue;

                deviceManager.setCurrentAudioDeviceType (type->getTypeName(), false);

                if (deviceManager.getCurrentAudioDevice() != nullptr)
                {
                    error.clear();
                    break;
                }
            }
        }
    }

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
    audioDeviceStopped();
}

void AudioEngine::prepareForTest (double sampleRate, int blockSize)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const int size = std::max (1, blockSize);
    workBuffer.setSize (2, size);
    workBuffer.clear();
    chain.prepare (sampleRate, size);
    muteGain.reset (sampleRate, ChainProcessor::kOutputDipSeconds);
    muteGain.setCurrentAndTargetValue (isOutputMuted() ? 0.0f : 1.0f);
    monoMix.reset (sampleRate, ChainProcessor::kOutputDipSeconds);
    monoMix.setCurrentAndTargetValue (isOutputMono() ? 1.0f : 0.0f);
    preparedBlockSize.store (size);
    chain.setProcessingActive (true);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
        prepareForTest (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped()
{
    preparedBlockSize = 0;
    chain.setProcessingActive (false);
    inputMeter.readAndReset();
    outputMeter.readAndReset();
}

void AudioEngine::audioDeviceError (const juce::String&)
{
    audioDeviceStopped();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Flush denormals to zero for the whole callback (no allocation).
    juce::ScopedNoDenormals noDenormals;

    // Not prepared: output silence.
    const int blockSize = preparedBlockSize.load();
    if (blockSize <= 0)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

        return;
    }

    const int selected = inputChannel.load();
    const float* source = (selected >= 0 && selected < numInputChannels) ? inputChannelData[selected] : nullptr;

    float* work[2] = { workBuffer.getWritePointer (0), workBuffer.getWritePointer (1) };

    muteGain.setTargetValue (outputMuted.load() ? 0.0f : 1.0f);
    monoMix.setTargetValue (outputMono.load() ? 1.0f : 0.0f);
    for (int start = 0; start < numSamples; start += blockSize)
    {
        const int n = std::min (blockSize, numSamples - start);

        if (source != nullptr)
            juce::FloatVectorOperations::copy (work[0], source + start, n);
        else
            juce::FloatVectorOperations::clear (work[0], n);

        juce::FloatVectorOperations::copy (work[1], work[0], n);

        // A view onto the work buffer: no allocation.
        juce::AudioBuffer<float> view (work, 2, n);

        inputMeter.pushPeak (view.getMagnitude (0, 0, n));
        chain.process (view);
        for (int i = 0; i < n; ++i)
        {
            // Mono: crossfade each side towards the mid signal.
            const float mono = monoMix.getNextValue();

            if (mono >= 1.0f)
            {
                work[0][i] = work[1][i] = 0.5f * (work[0][i] + work[1][i]);
            }
            else if (mono > 0.0f)
            {
                const float mid = 0.5f * (work[0][i] + work[1][i]);
                work[0][i] += mono * (mid - work[0][i]);
                work[1][i] += mono * (mid - work[1][i]);
            }

            const float gain = muteGain.getNextValue();
            for (int ch = 0; ch < 2; ++ch)
                work[ch][i] = juce::exactlyEqual (gain, 0.0f) ? 0.0f : work[ch][i] * gain;
        }
        float peak = 0.0f;
        for (int ch = 0; ch < std::min (2, numOutputChannels); ++ch)
            if (outputChannelData[ch] != nullptr)
                peak = std::max (peak, view.getMagnitude (ch, 0, n));
        outputMeter.pushPeak (peak);

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
