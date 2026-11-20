#include "TestProcessors.h"

#include "audio/AudioEngine.h"

#include <cmath>

struct AudioEngineTests : public juce::UnitTest
{
    AudioEngineTests() : juce::UnitTest ("AudioEngineTests", "DAW") {}

    static constexpr int kBlock = 256;

    static float sine (int n, float amp) { return amp * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * n / 48000.0); }

    void runTest() override
    {
        testOutputMute();
        testDeviceErrorSettlesEdits();

        // The engine copies samples unchanged, so exact float comparison is
        // intended here (juce::exactlyEqual avoids -Wfloat-equal).
        beginTest ("mono input is copied to both outputs");
        {
            AudioEngine engine;
            engine.prepareForTest (48000.0, kBlock);
            engine.setInputChannel (1);

            juce::AudioBuffer<float> in (2, kBlock), out (2, kBlock);
            in.clear();
            for (int i = 0; i < kBlock; ++i)
                in.setSample (1, i, sine (i, 0.5f));

            run (engine, in, out);

            bool same = true;
            for (int i = 0; i < kBlock; ++i)
                same = same && juce::exactlyEqual (out.getSample (0, i), in.getSample (1, i))
                            && juce::exactlyEqual (out.getSample (1, i), in.getSample (1, i));
            expect (same, "L == R == input channel 1");
        }

        beginTest ("out-of-range input channel gives silence");
        {
            AudioEngine engine;
            engine.prepareForTest (48000.0, kBlock);
            engine.setInputChannel (3);

            juce::AudioBuffer<float> in (1, kBlock), out (2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                in.setSample (0, i, 0.5f);
            out.applyGain (0.0f);
            out.setSample (0, 0, 1.0f);

            run (engine, in, out);
            expectEquals (out.getMagnitude (0, kBlock), 0.0f);
        }

        beginTest ("meters report peaks");
        {
            AudioEngine engine;
            engine.prepareForTest (48000.0, kBlock);

            juce::AudioBuffer<float> in (1, kBlock), out (2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                in.setSample (0, i, sine (i, 0.5f));

            run (engine, in, out);
            expectWithinAbsoluteError (engine.getInputMeter().readAndReset(), 0.5f, 0.01f);
            expectEquals (engine.getInputMeter().readAndReset(), 0.0f);
            expectWithinAbsoluteError (engine.getOutputMeter().readAndReset(), 0.5f, 0.01f);
        }

        beginTest ("the callback processes the chain between the input and output meters");
        {
            AudioEngine engine;
            engine.prepareForTest (48000.0, kBlock);
            auto slot = makeSlot (1, std::make_unique<GainProcessor> (0.25f), 48000.0, kBlock);
            expect (engine.getChain().post ({ ChainCommandType::Insert, 1, 0, false, slot.get() }));
            slot.release();

            juce::AudioBuffer<float> in (1, kBlock), out (2, kBlock);
            juce::FloatVectorOperations::fill (in.getWritePointer (0), 0.8f, kBlock);
            for (int i = 0; i < 10; ++i)
                run (engine, in, out);

            engine.getInputMeter().readAndReset();
            engine.getOutputMeter().readAndReset();
            run (engine, in, out);
            expectWithinAbsoluteError (engine.getInputMeter().readAndReset(), 0.8f, 1.0e-5f);
            expectWithinAbsoluteError (engine.getOutputMeter().readAndReset(), 0.2f, 1.0e-5f);
            for (int ch = 0; ch < 2; ++ch)
                expectWithinAbsoluteError (out.getSample (ch, kBlock - 1), 0.2f, 1.0e-5f);

            engine.audioDeviceStopped();
            run (engine, in, out);
            expectEquals (out.getMagnitude (0, kBlock), 0.0f, "a stopped engine outputs silence");
            expectEquals (engine.getInputMeter().readAndReset(), 0.0f);
            expectEquals (engine.getOutputMeter().readAndReset(), 0.0f);
        }

        beginTest ("block larger than prepared is processed in chunks");
        {
            AudioEngine engine;
            engine.prepareForTest (48000.0, 64);

            juce::AudioBuffer<float> in (1, 200), out (2, 200);
            for (int i = 0; i < 200; ++i)
                in.setSample (0, i, sine (i, 0.5f));

            run (engine, in, out);
            bool same = true;
            for (int i = 0; i < 200; ++i)
                same = same && juce::exactlyEqual (out.getSample (1, i), in.getSample (0, i));
            expect (same, "all 200 samples passed through");
        }

        beginTest ("extra output channels are zeroed, mono output writes L");
        {
            AudioEngine engine;
            engine.prepareForTest (48000.0, kBlock);

            juce::AudioBuffer<float> in (1, kBlock), out (3, kBlock);
            for (int i = 0; i < kBlock; ++i)
                in.setSample (0, i, 0.5f);
            out.applyGain (0.0f);
            out.setSample (2, 0, 9.0f);
            run (engine, in, out);
            expectEquals (out.getMagnitude (2, 0, kBlock), 0.0f);

            juce::AudioBuffer<float> mono (1, kBlock);
            run (engine, in, mono);
            expectEquals (mono.getSample (0, 10), 0.5f);
        }
    }

    void testOutputMute()
    {
        beginTest ("output mute fades both ways, keeps processing, and meters actual output");
        class ProcessingCounter : public GainProcessor
        {
        public:
            ProcessingCounter() : GainProcessor (0.5f) {}
            using GainProcessor::processBlock;
            void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
            { ++blocks; GainProcessor::processBlock (b, m); }
            int blocks = 0;
        };
        AudioEngine engine;
        expect (! engine.isOutputMuted());
        engine.prepareForTest (48000.0, 64); // Also exercise multi-chunk fades.
        auto counter = std::make_unique<ProcessingCounter>();
        auto* raw = counter.get();
        auto slot = makeSlot (1, std::move (counter), 48000.0, 64);
        expect (engine.getChain().post ({ ChainCommandType::Insert, 1, 0, false, slot.get() }));
        slot.release();
        juce::AudioBuffer<float> in (1, kBlock), out (2, kBlock);
        std::vector<float> samples;
        int position = 0;
        const auto blocks = [&] (int count)
        {
            for (int b = 0; b < count; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                    in.setSample (0, i, sineSample (position++));
                run (engine, in, out);
                samples.insert (samples.end(), out.getReadPointer (0), out.getReadPointer (0) + kBlock);
            }
        };
        blocks (10);
        engine.setOutputMuted (true);
        expect (engine.isOutputMuted());
        blocks (4);
        for (int ch = 0; ch < 2; ++ch)
            expectEquals (out.getMagnitude (ch, 0, kBlock), 0.0f);
        engine.getInputMeter().readAndReset();
        engine.getOutputMeter().readAndReset();
        const int countBefore = raw->blocks;
        blocks (1);
        expect (raw->blocks > countBefore, "muted processors still run");
        expectGreaterThan (engine.getInputMeter().readAndReset(), 0.7f);
        expectEquals (engine.getOutputMeter().readAndReset(), 0.0f);
        engine.setOutputMuted (false);
        blocks (4);
        expectLessThan (maxStep (samples), 0.06f);
        for (int ch = 0; ch < 2; ++ch)
            expectWithinAbsoluteError (out.getSample (ch, kBlock - 1),
                                       0.5f * in.getSample (0, kBlock - 1), 1.0e-6f);

        beginTest ("mute set before startup is silent from the first sample, including restart");
        AudioEngine initiallyMuted;
        initiallyMuted.setOutputMuted (true);
        initiallyMuted.prepareForTest (48000.0, kBlock);
        run (initiallyMuted, in, out);
        expectEquals (out.getMagnitude (0, kBlock), 0.0f);
        expectEquals (initiallyMuted.getOutputMeter().readAndReset(), 0.0f);
        expectGreaterThan (initiallyMuted.getInputMeter().readAndReset(), 0.7f);
        initiallyMuted.audioDeviceStopped();
        initiallyMuted.prepareForTest (44100.0, 128);
        run (initiallyMuted, in, out);
        expectEquals (out.getMagnitude (0, kBlock), 0.0f);
    }

    void testDeviceErrorSettlesEdits()
    {
        beginTest ("device error without a stop callback silences IO and settles more than 64 edits");
        AudioEngine engine;
        engine.prepareForTest (48000.0, kBlock);
        engine.audioDeviceError ("simulated start failure");
        for (int i = 0; i < 128; ++i)
            expect (engine.getChain().post ({ ChainCommandType::SetBypass, 999, 0, true }));
        juce::AudioBuffer<float> in (1, kBlock), out (2, kBlock);
        juce::FloatVectorOperations::fill (in.getWritePointer (0), 0.5f, kBlock);
        run (engine, in, out);
        expectEquals (out.getMagnitude (0, kBlock), 0.0f);
    }

    static void run (AudioEngine& engine, juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out)
    {
        juce::AudioIODeviceCallbackContext ctx;
        engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), in.getNumChannels(),
                                                 out.getArrayOfWritePointers(), out.getNumChannels(),
                                                 in.getNumSamples(), ctx);
    }
};

static AudioEngineTests audioEngineTests;
