#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <memory>

// One effect in the chain: a plugin instance, a wet/dry crossfade and a dry
// delay line that matches the plugin's latency (so the crossfade never combs).
//
// Created and prepared on the message thread. Once it has been posted to the
// ChainProcessor, only the audio thread touches it until it comes back through
// ChainProcessor::popRetired().
struct ChainSlot
{
    int id = 0;
    std::unique_ptr<juce::AudioProcessor> processor;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wet;   // 0 = dry, 1 = wet
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay;
    int latencySamples = 0;
    bool bypassed = false;

    // Calls processor->prepareToPlay, reads its latency, prepares dryDelay and
    // preallocates every buffer process() needs. Message thread only, never while
    // the audio thread owns this slot. Defined in ChainProcessor.cpp (it uses
    // ChainProcessor::kSlotFadeSeconds).
    void prepare (double sampleRate, int maxBlockSize);

    //==========================================================================
    // Audio-thread state below. Managed by ChainProcessor.

    // Structural change waiting for this slot's wet to reach 0: Remove (leave
    // the chain) or Move (be re-placed to match the chain's logical order).
    // While one is pending, the wet target is 0 regardless of `bypassed`.
    enum class PendingOp { None, Remove, Move };
    PendingOp pendingOp = PendingOp::None;

    // A slot with latency is placed in the chain at once but is not processed
    // (and its latency not counted) until the output dip has reached 0. Until
    // then it is an exact pass-through.
    bool awaitingDip = false;

    // The wet value this slot should be heading towards right now.
    float targetWet() const noexcept
    {
        return (bypassed || pendingOp != PendingOp::None) ? 0.0f : 1.0f;
    }

    // Preallocated in prepare(): the plugin processes a copy of the input here.
    juce::AudioBuffer<float> processBuffer;
    juce::MidiBuffer midi;
    int preparedBlockSize = 0;
};
