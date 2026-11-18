#pragma once

// Test-only AudioProcessors and helpers for driving ChainProcessor without an
// audio device.

#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/ChainProcessor.h"
#include "audio/ChainSlot.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

//==============================================================================
// Stereo in / stereo out, no editor, no programs, no state.
class TestProcessorBase : public juce::AudioProcessor
{
public:
    TestProcessorBase()
        : TestProcessorBase (BusesProperties()
                                 .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                 .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    explicit TestProcessorBase (const BusesProperties& buses) : juce::AudioProcessor (buses) {}

    const juce::String getName() const override              { return "TestProcessor"; }
    void prepareToPlay (double, int) override                {}
    void releaseResources() override                         {}
    double getTailLengthSeconds() const override             { return 0.0; }
    bool acceptsMidi() const override                        { return false; }
    bool producesMidi() const override                       { return false; }
    juce::AudioProcessorEditor* createEditor() override      { return nullptr; }
    bool hasEditor() const override                          { return false; }
    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override   {}
    void setStateInformation (const void*, int) override     {}

    using juce::AudioProcessor::processBlock;
};

// out = -in
class InvertProcessor : public TestProcessorBase
{
public:
    const juce::String getName() const override { return "Invert"; }

    using TestProcessorBase::processBlock;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (-1.0f);
    }
};

// out = gain * in
class GainProcessor : public TestProcessorBase
{
public:
    explicit GainProcessor (float g) : gain (g) {}

    const juce::String getName() const override { return "Gain"; }

    using TestProcessorBase::processBlock;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (gain);
    }

private:
    float gain;
};

// out[n] = in[n - samples], and reports `samples` as its latency.
class FixedDelayProcessor : public TestProcessorBase
{
public:
    explicit FixedDelayProcessor (int samples) : delaySamples (samples)
    {
        setLatencySamples (delaySamples);
    }

    const juce::String getName() const override { return "FixedDelay"; }

    using TestProcessorBase::processBlock;

    void prepareToPlay (double, int) override
    {
        history.setSize (2, juce::jmax (1, delaySamples));
        history.clear();
        writePos = 0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (delaySamples <= 0)
            return;

        const int numChannels = juce::jmin (buffer.getNumChannels(), history.getNumChannels());

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                auto* hist = history.getWritePointer (ch);
                const float delayed = hist[writePos];
                hist[writePos] = data[i];
                data[i] = delayed;
            }

            writePos = (writePos + 1) % delaySamples;
        }
    }

private:
    int delaySamples;
    juce::AudioBuffer<float> history;
    int writePos = 0;
};

// Only accepts a mono in / mono out layout.
class MonoOnlyProcessor : public TestProcessorBase
{
public:
    MonoOnlyProcessor()
        : TestProcessorBase (BusesProperties()
                                 .withInput ("Input", juce::AudioChannelSet::mono(), true)
                                 .withOutput ("Output", juce::AudioChannelSet::mono(), true))
    {
    }

    const juce::String getName() const override { return "MonoOnly"; }

    using TestProcessorBase::processBlock;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
};

//==============================================================================
// Builds a prepared slot, ready to be posted with ChainCommandType::Insert.
inline std::unique_ptr<ChainSlot> makeSlot (int id, std::unique_ptr<juce::AudioProcessor> processor,
                                            double sr, int block)
{
    auto slot = std::make_unique<ChainSlot>();
    slot->id = id;
    slot->processor = std::move (processor);
    slot->prepare (sr, block);
    return slot;
}

// The sine runSine feeds a chain is continuous across calls on the same
// ChainProcessor, so outputs of consecutive calls can be concatenated and
// checked with maxStep. Sample n of that signal is sineSample(n, ...).
//
// The signal rides on a DC offset larger than the sine's amplitude, so it never
// comes near zero (it stays within [0.25, 0.75] by default). That makes click
// detection independent of the sine's phase: any hard (unfaded) switch, such
// as x -> -x, x -> 0 or x -> 0.5x, jumps by at least 0.125, more than twice
// the 0.06 click threshold, wherever in the sine it lands.
constexpr float kTestSignalOffset = 0.5f;

inline std::map<const ChainProcessor*, std::int64_t>& sinePositions()
{
    static std::map<const ChainProcessor*, std::int64_t> positions;
    return positions;
}

// Absolute index of the next sample runSine will feed into `chain`.
inline std::int64_t sinePosition (const ChainProcessor& chain)
{
    return sinePositions()[&chain];
}

inline float sineSample (std::int64_t n, float amp = 0.25f, double freq = 440.0, double sr = 48000.0)
{
    return kTestSignalOffset
         + amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * (double) n / sr);
}

// Feeds numBlocks blocks of a stereo (L = R) offset sine through `chain` and
// returns the accumulated left-channel output.
inline std::vector<float> runSine (ChainProcessor& chain, int numBlocks, float amp = 0.25f,
                                   double freq = 440.0, double sr = 48000.0, int block = 256)
{
    auto& pos = sinePositions()[&chain];

    std::vector<float> out;
    out.reserve ((size_t) (numBlocks * block));

    juce::AudioBuffer<float> buffer (2, block);

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const float s = sineSample (pos + i, amp, freq, sr);
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
        }

        pos += block;
        chain.process (buffer);

        for (int i = 0; i < block; ++i)
            out.push_back (buffer.getSample (0, i));
    }

    return out;
}

// Largest absolute difference between adjacent samples.
inline float maxStep (const std::vector<float>& samples)
{
    float result = 0.0f;

    for (size_t i = 1; i < samples.size(); ++i)
        result = juce::jmax (result, std::abs (samples[i] - samples[i - 1]));

    return result;
}
