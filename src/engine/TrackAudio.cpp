#include "TrackAudio.h"

#include <algorithm>

void TrackAudio::prepare (double sampleRate, int maxBlockSize)
{
    maxBlockSize = std::max (1, maxBlockSize);
    buffer.setSize (2, maxBlockSize);
    buffer.clear();
    delayBuffer.setSize (2, kMaxCompensation + maxBlockSize + 1);
    delayBuffer.clear();
    delayWrite = 0;
    chain.prepare (sampleRate, maxBlockSize);

    gainL.reset (sampleRate, kGainRampSeconds);
    gainR.reset (sampleRate, kGainRampSeconds);
    gainL.setCurrentAndTargetValue (targetGain (false) * std::min (1.0f, 1.0f - getPan()));
    gainR.setCurrentAndTargetValue (targetGain (false) * std::min (1.0f, 1.0f + getPan()));
}

float TrackAudio::targetGain (bool silenced) const noexcept
{
    if (silenced || isMuted())
        return 0.0f;

    const float db = getVolumeDb();
    return db <= kMinDb ? 0.0f : juce::Decibels::decibelsToGain (db, kMinDb);
}

void TrackAudio::processInserts (int n) noexcept
{
    auto* const* channels = buffer.getArrayOfWritePointers();
    juce::AudioBuffer<float> view (channels, 2, n);   // no allocation
    chain.process (view);

    // Delay compensation. The ring buffer is always written, even with no
    // delay, so its history is real audio when the delay is changed.
    const int size = delayBuffer.getNumSamples();
    for (int ch = 0; ch < 2; ++ch)
    {
        float* ring = delayBuffer.getWritePointer (ch);
        float* data = channels[ch];
        int w = delayWrite;

        for (int i = 0; i < n; ++i)
        {
            ring[w] = data[i];
            int r = w - compensation;
            if (r < 0)
                r += size;
            data[i] = ring[r];
            if (++w == size)
                w = 0;
        }
    }

    delayWrite = (delayWrite + n) % size;
}

void TrackAudio::mixInto (float* const* dest, int n, bool silenced) noexcept
{
    const float gain = targetGain (silenced);
    const float p = getPan();
    // Balance pan: centre is unity on both sides, a hard pan removes the other side.
    gainL.setTargetValue (gain * std::min (1.0f, 1.0f - p));
    gainR.setTargetValue (gain * std::min (1.0f, 1.0f + p));

    const float* left = buffer.getReadPointer (0);
    const float* right = buffer.getReadPointer (1);
    float peakL = 0.0f, peakR = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float l = left[i] * gainL.getNextValue();
        const float r = right[i] * gainR.getNextValue();
        dest[0][i] += l;
        dest[1][i] += r;
        peakL = std::max (peakL, std::abs (l));
        peakR = std::max (peakR, std::abs (r));
    }

    meterL.pushPeak (peakL);
    meterR.pushPeak (peakR);
}

void TrackAudio::applyFaderInPlace (int n) noexcept
{
    const float gain = targetGain (false);
    gainL.setTargetValue (gain);
    gainR.setTargetValue (gain);

    float* left = buffer.getWritePointer (0);
    float* right = buffer.getWritePointer (1);
    float peakL = 0.0f, peakR = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        left[i] *= gainL.getNextValue();
        right[i] *= gainR.getNextValue();
        peakL = std::max (peakL, std::abs (left[i]));
        peakR = std::max (peakR, std::abs (right[i]));
    }

    meterL.pushPeak (peakL);
    meterR.pushPeak (peakR);
}
