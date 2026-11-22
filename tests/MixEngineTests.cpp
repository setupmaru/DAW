#include "TestProcessors.h"

#include "engine/MixEngine.h"
#include "session/Session.h"

#include <cmath>

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

AudioDataPtr makeConstant (int length, float value, int channels = 1)
{
    juce::AudioBuffer<float> buffer (channels, length);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value, length);
    return makeAudioData (std::move (buffer), kRate);
}

Clip makeClip (int id, juce::int64 start, AudioDataPtr data)
{
    Clip c;
    c.id = id;
    c.data = data;
    c.start = start;
    c.length = data->getLength();
    return c;
}

// Runs the engine for `numSamples`, block by block, and returns the stereo output.
juce::AudioBuffer<float> run (MixEngine& engine, int numSamples, const juce::AudioBuffer<float>* input = nullptr)
{
    juce::AudioBuffer<float> out (2, numSamples);
    out.clear();
    juce::AudioBuffer<float> block (2, kBlock);
    juce::AudioBuffer<float> silentIn (2, kBlock);
    silentIn.clear();

    for (int pos = 0; pos < numSamples; pos += kBlock)
    {
        const int n = std::min (kBlock, numSamples - pos);
        const float* in[2] = { silentIn.getReadPointer (0), silentIn.getReadPointer (1) };
        if (input != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                in[ch] = input->getReadPointer (std::min (ch, input->getNumChannels() - 1), pos);

        engine.audioDeviceIOCallbackWithContext (in, 2, block.getArrayOfWritePointers(), 2, n, {});
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, n);
    }

    return out;
}

void publish (MixEngine& engine, std::vector<std::pair<std::shared_ptr<TrackAudio>, std::vector<Clip>>> tracks)
{
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    for (auto& [audio, clips] : tracks)
        snapshot->addTrack (audio, clips);
    engine.publish (std::move (snapshot));
}
} // namespace

struct MixEngineTests : public juce::UnitTest
{
    MixEngineTests() : juce::UnitTest ("MixEngineTests", "DAW") {}

    void runTest() override
    {
        testPlayback();
        testFades();
        testMuteSoloPan();
        testLoop();
        testInputMonitoring();
        testSnapshotChurn();
        testRecording();
        testDelayCompensation();
    }

    void testPlayback()
    {
        beginTest ("clips play at their timeline position, only while playing");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        auto track = engine.createTrackAudio();
        publish (engine, { { track, { makeClip (1, 300, makeConstant (1000, 0.5f)) } } });

        auto stopped = run (engine, 2048);
        expectEquals (stopped.getMagnitude (0, 2048), 0.0f, "stopped: silence");
        expectEquals ((int) engine.getPosition(), 0);

        expect (engine.play());
        auto out = run (engine, 2048);
        expectEquals (out.getMagnitude (0, 0, 300), 0.0f, "before the clip");
        expectWithinAbsoluteError (out.getSample (0, 300), 0.5f, 1.0e-6f);
        expectWithinAbsoluteError (out.getSample (1, 1299), 0.5f, 1.0e-6f, "mono clip on both sides");
        expectEquals (out.getMagnitude (0, 1300, 748), 0.0f, "after the clip");
        expectEquals ((int) engine.getPosition(), 2048);

        engine.stop();
        run (engine, kBlock);
        expect (! engine.isPlaying());
        engine.locate (1000);
        engine.play();
        auto located = run (engine, kBlock);
        expectWithinAbsoluteError (located.getSample (0, 0), 0.5f, 1.0e-6f, "locate then play");
    }

    void testFades()
    {
        beginTest ("fade in and out are linear inside the clip");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        auto track = engine.createTrackAudio();
        auto clip = makeClip (1, 0, makeConstant (1000, 1.0f));
        clip.fadeIn = 100;
        clip.fadeOut = 200;
        publish (engine, { { track, { clip } } });
        engine.play();
        auto out = run (engine, 1024);
        expectWithinAbsoluteError (out.getSample (0, 0), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (out.getSample (0, 50), 0.5f, 1.0e-6f);
        expectWithinAbsoluteError (out.getSample (0, 500), 1.0f, 1.0e-6f);
        expectWithinAbsoluteError (out.getSample (0, 900), 0.5f, 1.0e-6f);
        expectWithinAbsoluteError (out.getSample (0, 999), 1.0f / 200.0f, 1.0e-6f);
    }

    void testMuteSoloPan()
    {
        beginTest ("mute, solo and pan ramp without clicks and end where expected");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        auto a = engine.createTrackAudio();
        auto b = engine.createTrackAudio();
        publish (engine, { { a, { makeClip (1, 0, makeConstant (48000, 0.25f)) } },
                           { b, { makeClip (2, 0, makeConstant (48000, 0.5f)) } } });
        engine.play();
        auto both = run (engine, 1024);
        expectWithinAbsoluteError (both.getSample (0, 1000), 0.75f, 1.0e-6f);

        b->setSolo (true);
        auto solo = run (engine, 2048);
        expectWithinAbsoluteError (solo.getSample (0, 2000), 0.5f, 1.0e-5f, "only the soloed track");
        float maxStep = 0.0f;
        for (int i = 1; i < 2048; ++i)
            maxStep = std::max (maxStep, std::abs (solo.getSample (0, i) - solo.getSample (0, i - 1)));
        expect (maxStep < 0.01f, "solo ramps (step " + juce::String (maxStep) + ")");

        b->setSolo (false);
        a->setMute (true);
        b->setPan (-1.0f);
        auto panned = run (engine, 2048);
        expectWithinAbsoluteError (panned.getSample (0, 2000), 0.5f, 1.0e-5f, "hard left keeps the left side");
        expectWithinAbsoluteError (panned.getSample (1, 2000), 0.0f, 1.0e-5f, "and removes the right side");

        b->setVolumeDb (-6.0f);
        b->setPan (0.0f);
        auto quieter = run (engine, 2048);
        expectWithinAbsoluteError (quieter.getSample (1, 2000), 0.5f * juce::Decibels::decibelsToGain (-6.0f), 1.0e-5f);
    }

    void testLoop()
    {
        beginTest ("loop playback wraps between the loop points");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        auto track = engine.createTrackAudio();
        juce::AudioBuffer<float> ramp (1, 1000);
        for (int i = 0; i < 1000; ++i)
            ramp.setSample (0, i, (float) i);
        publish (engine, { { track, { makeClip (1, 0, makeAudioData (std::move (ramp), kRate)) } } });

        engine.setLoop (true, 100, 400);
        engine.locate (100);
        engine.play();
        auto out = run (engine, 1000);
        expectWithinAbsoluteError (out.getSample (0, 0), 100.0f, 1.0e-3f);
        expectWithinAbsoluteError (out.getSample (0, 299), 399.0f, 1.0e-3f);
        expectWithinAbsoluteError (out.getSample (0, 300), 100.0f, 1.0e-3f, "wrapped to the loop start");
        expect (engine.getPosition() >= 100 && engine.getPosition() < 400);
    }

    void testInputMonitoring()
    {
        beginTest ("an armed track monitors its input while stopped and plays clips while playing");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        auto track = engine.createTrackAudio();
        track->setArmed (true);
        track->setInput (1, false);
        publish (engine, { { track, { makeClip (1, 0, makeConstant (4096, 0.25f)) } } });

        juce::AudioBuffer<float> input (2, 1024);
        input.clear();
        juce::FloatVectorOperations::fill (input.getWritePointer (1), 0.5f, 1024);

        auto stopped = run (engine, 1024, &input);
        expectWithinAbsoluteError (stopped.getSample (0, 500), 0.5f, 1.0e-6f, "input 2 on both sides");
        expectWithinAbsoluteError (stopped.getSample (1, 500), 0.5f, 1.0e-6f);

        engine.play();
        auto playing = run (engine, 1024, &input);
        expectWithinAbsoluteError (playing.getSample (0, 500), 0.25f, 1.0e-6f, "playback, not input");
    }

    void testSnapshotChurn()
    {
        beginTest ("snapshots can be replaced every block; old ones are freed on the message thread");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        auto track = engine.createTrackAudio();
        auto data = makeConstant (100000, 0.5f);
        engine.play();

        for (int i = 0; i < 500; ++i)
        {
            publish (engine, { { track, { makeClip (1, 0, data) } } });
            publish (engine, { { track, { makeClip (1, 0, data) } } });   // replaces an unseen one
            run (engine, kBlock);
            if (i % 10 == 0)
                engine.collectGarbage();
        }

        engine.collectGarbage();
        expect (data.use_count() <= 3, "no snapshot is leaked (" + juce::String ((int) data.use_count()) + ")");
    }

    void testRecording()
    {
        beginTest ("recording writes the armed track's input into a new clip at the record position");
        auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("daw-rec", "");
        folder.createDirectory();

        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        juce::AudioPluginFormatManager plugins;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::StringArray errors;
        {
            Session session (engine, plugins, formats);
            session.onError = [&] (const juce::String& e) { errors.add (e); };
            session.setFile (folder.getChildFile ("test.dawsession"));
            auto* track = session.addTrack ("Vocal");
            session.setTrackArmed (track->id, true);
            session.setTrackInput (track->id, 0, false);

            engine.locate (4800);
            run (engine, kBlock);
            expectEquals (session.startRecording(), juce::String());

            const int length = 48000;
            juce::AudioBuffer<float> input (1, length);
            for (int i = 0; i < length; ++i)
                input.setSample (0, i, 0.5f * (float) std::sin (0.01 * i));
            run (engine, length, &input);

            engine.stop();
            engine.locate (4800);   // right after stop: applied with the end of the take
            run (engine, kBlock);
            expect (! engine.isRecording());
            expectEquals ((int) engine.getPosition(), 4800, "the playhead returns after recording");

            for (int i = 0; i < 200 && track->clips.empty(); ++i)
            {
                session.pumpForTest();
                juce::Thread::sleep (5);
            }

            expectEquals ((int) track->clips.size(), 1);
            if (! track->clips.empty())
            {
                const auto& clip = track->clips.front();
                expectEquals ((int) clip.start, 4800);
                expectEquals ((int) clip.length, length);
                expect (clip.data->file.isAChildOf (folder.getChildFile ("Audio Files")));
                float maxError = 0.0f;
                for (int i = 0; i < length; ++i)
                    maxError = std::max (maxError, std::abs (clip.data->samples.getSample (0, (int) clip.offset + i) - input.getSample (0, i)));
                expect (maxError < 1.0e-5f, "recorded samples match the input (" + juce::String (maxError) + ")");
            }

            expect (session.getUndoManager().canUndo(), "recording is undoable");
        }
        expect (errors.isEmpty(), errors.joinIntoString ("; "));
        folder.deleteRecursively();
    }

    void testDelayCompensation()
    {
        beginTest ("tracks with different insert latency stay aligned");
        MixEngine engine;
        engine.prepareForTest (kRate, kBlock);
        juce::AudioPluginFormatManager plugins;
        juce::AudioFormatManager formats;
        Session session (engine, plugins, formats);
        auto* a = session.addTrack();
        auto* b = session.addTrack();
        expect (a->getInserts().addProcessor (std::make_unique<FixedDelayProcessor> (64), "Delay"));

        juce::AudioBuffer<float> impulse (1, 16);
        impulse.clear();
        impulse.setSample (0, 0, 1.0f);
        auto data = makeAudioData (std::move (impulse), kRate);

        Session::ClipMap clips;
        clips[a->id] = { makeClip (1, 24000, data) };
        clips[b->id] = { makeClip (2, 24000, data) };
        session.applyClips (clips);

        engine.play();
        auto out = run (engine, 48000);
        expectEquals (engine.getMixLatency(), 64);
        expectEquals (a->getAudio().getCompensation(), 0);
        expectEquals (b->getAudio().getCompensation(), 64);

        int peakIndex = -1;
        for (int i = 0; i < 48000; ++i)
            if (out.getSample (0, i) > 1.5f)
                peakIndex = i;
        expectEquals (peakIndex, 24000 + 64, "both impulses on the same sample, one latency late");
    }
};

static MixEngineTests mixEngineTests;
