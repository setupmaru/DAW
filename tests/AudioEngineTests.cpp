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

    static void run (AudioEngine& engine, juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out)
    {
        juce::AudioIODeviceCallbackContext ctx;
        engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), in.getNumChannels(),
                                                 out.getArrayOfWritePointers(), out.getNumChannels(),
                                                 in.getNumSamples(), ctx);
    }
};

static AudioEngineTests audioEngineTests;
