#include "TestProcessors.h"

#include "session/Bouncer.h"
#include "session/Session.h"
#include "session/SessionFile.h"

namespace
{
constexpr double kRate = 48000.0;

// Writes a mono 24-bit WAV of `length` samples of `value`.
juce::File writeWav (const juce::File& file, int length, float value)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (kRate)
                                                   .withNumChannels (1).withBitsPerSample (24));
    juce::AudioBuffer<float> buffer (1, length);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), value, length);
    writer->writeFromAudioSampleBuffer (buffer, 0, length);
    return file;
}

struct Fixture
{
    Fixture()
    {
        formats.registerBasicFormats();
        engine.prepareForTest (kRate, 256);
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("daw-session", "");
        folder.createDirectory();
    }

    ~Fixture()
    {
        session.reset();
        folder.deleteRecursively();
    }

    MixEngine engine;
    juce::AudioPluginFormatManager plugins;
    juce::AudioFormatManager formats;
    std::unique_ptr<Session> session = std::make_unique<Session> (engine, plugins, formats);
    juce::File folder;
};
} // namespace

struct SessionTests : public juce::UnitTest
{
    SessionTests() : juce::UnitTest ("SessionTests", "DAW") {}

    void runTest() override
    {
        testEditsAndUndo();
        testSaveAndLoad();
        testBounce();
        testDuplicateAcrossTracks();
        testSaveWithMissingSource();
        testResetDuringRecording();
    }

    void testEditsAndUndo()
    {
        beginTest ("import, split, delete, duplicate and undo/redo");
        Fixture f;
        auto& s = *f.session;
        auto* track = s.addTrack ("Gtr");
        const auto source = writeWav (f.folder.getChildFile ("source.wav"), 10000, 0.5f);

        juce::String error;
        const int clipId = s.importAudioFile (source, track->id, 1000, error);
        expect (clipId > 0, error);
        expectEquals ((int) track->clips.size(), 1);
        expectEquals ((int) s.getLength(), 11000);

        s.splitClips ({ track->id }, 5000);
        expectEquals ((int) track->clips.size(), 2);

        s.getUndoManager().undo();
        expectEquals ((int) track->clips.size(), 1, "undo split");
        s.getUndoManager().redo();
        expectEquals ((int) track->clips.size(), 2, "redo split");

        const auto copies = s.duplicateClips ({ track->clips[1].id });
        expectEquals ((int) copies.size(), 1);
        expectEquals ((int) track->clips.size(), 3);
        expectEquals ((int) track->clips[2].start, 11000, "right after the original");

        s.deleteRange ({ track->id }, 2000, 12000);
        expectEquals ((int) track->clips.size(), 2);
        expectEquals ((int) track->clips[0].end(), 2000);
        expectEquals ((int) track->clips[1].start, 12000);

        s.deleteClips ({ track->clips[0].id });
        expectEquals ((int) track->clips.size(), 1);

        while (s.getUndoManager().canUndo())
            s.getUndoManager().undo();
        expect (track->clips.empty(), "back to before the import");
        expect (s.isDirty());
    }

    void testSaveAndLoad()
    {
        beginTest ("a saved session reopens with its tracks, clips and mixer settings");
        Fixture f;
        auto& s = *f.session;
        auto* a = s.addTrack ("Kick");
        auto* b = s.addTrack ("Snare");
        s.setTempo (96.0);
        s.setTrackVolume (a->id, -3.5f);
        s.setTrackPan (b->id, 0.25f);
        s.setTrackMute (b->id, true);
        s.setTrackInput (b->id, 2, true);
        s.setTrackInputMonitor (b->id, true);
        s.setMasterVolume (-1.0f);

        // The source is outside the session folder: saving copies it in.
        const auto outside = f.folder.getChildFile ("outside");
        outside.createDirectory();
        const auto source = writeWav (outside.getChildFile ("kick.wav"), 4800, 0.25f);
        juce::String error;
        s.importAudioFile (source, a->id, 480, error);
        s.importAudioFile (source, b->id, 9600, error);
        s.splitClips ({ b->id }, 12000);
        b->clips[0].fadeIn = 100;   // a direct model change, saved as is
        s.applyClips (s.captureClips());

        const auto file = f.folder.getChildFile ("song").getChildFile ("song.dawsession");
        file.getParentDirectory().createDirectory();
        expectEquals (SessionFile::save (s, file), juce::String());
        expect (! s.isDirty());
        expect (file.getSiblingFile ("Audio Files").getChildFile ("kick.wav").existsAsFile(), "audio copied in");
        expect (source.existsAsFile(), "the original stays");

        bool done = false;
        juce::StringArray problems;
        Fixture g;
        expect (SessionFile::load (*g.session, file, error, [&] (juce::StringArray p) { problems = p; done = true; }), error);
        for (int i = 0; i < 100 && ! done; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        expect (done);
        expect (problems.isEmpty(), problems.joinIntoString ("; "));

        auto& t = *g.session;
        expectEquals ((int) t.getTracks().size(), 2);
        expectEquals (t.getTempo(), 96.0);
        expectEquals (t.getMasterVolume(), -1.0f);
        const auto& ka = *t.getTracks()[0];
        const auto& kb = *t.getTracks()[1];
        expectEquals (ka.name, juce::String ("Kick"));
        expectEquals (ka.volumeDb, -3.5f);
        expectEquals (kb.pan, 0.25f);
        expect (kb.mute && kb.stereoInput && kb.inputChannel == 2);
        expect (kb.inputMonitor && ! ka.inputMonitor, "input monitor is saved");
        expect (kb.getAudioPtr()->isInputMonitor(), "and reaches the engine");
        expectEquals ((int) ka.clips.size(), 1);
        expectEquals ((int) ka.clips[0].start, 480);
        expectEquals ((int) kb.clips.size(), 2);
        expectEquals ((int) kb.clips[1].start, 12000);
        expectEquals ((int) kb.clips[1].offset, 2400);
        expectEquals ((int) kb.clips[0].fadeIn, 100);
        expect (ka.clips[0].data == kb.clips[0].data, "one file loads once");
        expectWithinAbsoluteError (ka.clips[0].data->samples.getSample (0, 100), 0.25f, 1.0e-5f);
        expect (! t.isDirty());
        expect (! t.getUndoManager().canUndo());
    }

    void testBounce()
    {
        beginTest ("bounce renders the mix through faders into a WAV that lines up with the timeline");
        Fixture f;
        auto& s = *f.session;
        auto* a = s.addTrack();
        juce::String error;
        s.importAudioFile (writeWav (f.folder.getChildFile ("a.wav"), 4800, 0.5f), a->id, 2400, error);
        s.setTrackVolume (a->id, -6.0f);
        expect (a->getInserts().addProcessor (std::make_unique<FixedDelayProcessor> (100), "Delay"));

        const auto target = f.folder.getChildFile ("mix.wav");
        {
            Bouncer bouncer (s, target, 0, 9600);
            expectEquals (bouncer.renderNow(), juce::String());
        }
        expect (! f.engine.isOffline(), "the device is reattached");

        std::unique_ptr<juce::AudioFormatReader> reader (f.formats.createReaderFor (target));
        expect (reader != nullptr);
        if (reader == nullptr)
            return;

        expectEquals ((int) reader->lengthInSamples, 9600);
        juce::AudioBuffer<float> mix (2, 9600);
        reader->read (&mix, 0, 9600, 0, true, true);
        const float expected = 0.5f * juce::Decibels::decibelsToGain (-6.0f);
        expectWithinAbsoluteError (mix.getSample (0, 2399), 0.0f, 1.0e-4f, "silent before the clip");
        expectWithinAbsoluteError (mix.getSample (0, 2400), expected, 1.0e-4f, "starts on time despite the plugin delay");
        expectWithinAbsoluteError (mix.getSample (1, 7199), expected, 1.0e-4f);
        expectWithinAbsoluteError (mix.getSample (1, 7200), 0.0f, 1.0e-4f);
    }

    void testDuplicateAcrossTracks()
    {
        beginTest ("duplicate moves a selection on several tracks as one block");
        Fixture f;
        auto& s = *f.session;
        auto* a = s.addTrack();
        auto* b = s.addTrack();
        const auto source = writeWav (f.folder.getChildFile ("d.wav"), 1000, 0.5f);
        juce::String error;
        const int first = s.importAudioFile (source, a->id, 0, error);
        const int second = s.importAudioFile (source, b->id, 2000, error);   // ends at 3000
        const auto created = s.duplicateClips ({ first, second });
        expectEquals ((int) created.size(), 2);
        expectEquals ((int) a->clips.back().start, 3000, "same shift on every track");
        expectEquals ((int) b->clips.back().start, 5000);
    }

    void testSaveWithMissingSource()
    {
        beginTest ("saving still works when an imported file was deleted from disk");
        Fixture f;
        auto& s = *f.session;
        auto* a = s.addTrack();
        const auto source = writeWav (f.folder.getChildFile ("gone.wav"), 2400, 0.25f);
        juce::String error;
        s.importAudioFile (source, a->id, 0, error);
        expect (source.deleteFile());

        const auto file = f.folder.getChildFile ("s").getChildFile ("s.dawsession");
        file.getParentDirectory().createDirectory();
        expectEquals (SessionFile::save (s, file), juce::String());
        const auto saved = a->clips[0].data->file;
        expect (saved.existsAsFile() && saved.isAChildOf (file.getParentDirectory()), "written from memory into Audio Files");

        std::unique_ptr<juce::AudioFormatReader> reader (f.formats.createReaderFor (saved));
        expect (reader != nullptr);
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> buffer (1, 2400);
            reader->read (&buffer, 0, 2400, 0, true, false);
            expectWithinAbsoluteError (buffer.getSample (0, 1000), 0.25f, 1.0e-5f);
        }
    }

    void testResetDuringRecording()
    {
        beginTest ("a recording still running at New/Open never lands in the new session; ids are not reused");
        Fixture f;
        auto& s = *f.session;
        s.setFile (f.folder.getChildFile ("r.dawsession"));
        auto* a = s.addTrack();
        const int oldId = a->id;
        s.setTrackArmed (a->id, true);
        expectEquals (s.startRecording(), juce::String());

        juce::AudioBuffer<float> in (1, 256), out (2, 256);
        juce::FloatVectorOperations::fill (in.getWritePointer (0), 0.5f, 256);
        for (int i = 0; i < 8; ++i)
            f.engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 2, 256, {});

        s.reset (kRate);              // New session while recording ("don't save")
        auto* fresh = s.addTrack();
        expect (fresh->id != oldId, "track ids are not reused after reset");
        s.setDirty (false);   // as after New: only the take could dirty it now
        f.engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 2, 256, {});
        for (int i = 0; i < 50; ++i)
        {
            s.pumpForTest();
            juce::Thread::sleep (2);
        }

        expect (fresh->clips.empty(), "the old take is dropped");
        expect (! s.getUndoManager().canUndo());
        expect (! s.isDirty());
    }
};

static SessionTests sessionTests;
