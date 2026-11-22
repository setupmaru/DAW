#include <juce_core/juce_core.h>

#include "engine/Clip.h"

namespace
{
AudioDataPtr makeRamp (int length)
{
    juce::AudioBuffer<float> buffer (1, length);
    for (int i = 0; i < length; ++i)
        buffer.setSample (0, i, (float) i / (float) length);
    return makeAudioData (std::move (buffer), 48000.0);
}

Clip makeClip (int id, juce::int64 start, juce::int64 length, AudioDataPtr data, juce::int64 offset = 0)
{
    Clip c;
    c.id = id;
    c.name = "c" + juce::String (id);
    c.data = std::move (data);
    c.start = start;
    c.offset = offset;
    c.length = length;
    return c;
}
} // namespace

struct ClipEditTests : public juce::UnitTest
{
    ClipEditTests() : juce::UnitTest ("ClipEditTests", "DAW") {}

    void runTest() override
    {
        auto data = makeRamp (10000);
        int nextId = 100;
        const ClipEdit::IdSource ids = [&] { return nextId++; };

        beginTest ("splitAt cuts one clip into two that play the same audio");
        {
            std::vector<Clip> clips { makeClip (1, 1000, 4000, data, 500) };
            clips[0].fadeIn = 200;
            clips[0].fadeOut = 300;
            const int right = ClipEdit::splitAt (clips, 3000, ids);
            expectEquals ((int) clips.size(), 2);
            expectEquals (right, 100);
            expectEquals (clips[0].id, 1);
            expect (clips[0].start == 1000 && clips[0].length == 2000 && clips[0].offset == 500);
            expect (clips[0].fadeIn == 200 && clips[0].fadeOut == 0, "cut edge has no fade");
            expect (clips[1].start == 3000 && clips[1].length == 2000 && clips[1].offset == 2500);
            expect (clips[1].fadeIn == 0 && clips[1].fadeOut == 300);
            expectEquals (ClipEdit::splitAt (clips, 1000, ids), 0, "an edge is not a split");
            expectEquals (ClipEdit::splitAt (clips, 20000, ids), 0, "outside any clip");
        }

        beginTest ("clearRange shortens, splits and removes");
        {
            std::vector<Clip> clips { makeClip (1, 0, 1000, data), makeClip (2, 2000, 1000, data), makeClip (3, 4000, 2000, data) };
            ClipEdit::clearRange (clips, 500, 2500, ids);   // cuts the end of 1 and the start of 2
            expectEquals ((int) clips.size(), 3);
            expect (clips[0].id == 1 && clips[0].length == 500);
            expect (clips[1].id == 2 && clips[1].start == 2500 && clips[1].offset == 500 && clips[1].length == 500);

            ClipEdit::clearRange (clips, 4500, 5000, ids);  // inside clip 3: split
            expectEquals ((int) clips.size(), 4);
            expect (clips[2].id == 3 && clips[2].start == 4000 && clips[2].length == 500);
            expect (clips[3].start == 5000 && clips[3].offset == 1000 && clips[3].length == 1000 && clips[3].id != 3);

            ClipEdit::clearRange (clips, 0, 100000, ids);   // everything
            expect (clips.empty());
        }

        beginTest ("place overwrites what is under the new clip");
        {
            std::vector<Clip> clips { makeClip (1, 0, 3000, data) };
            ClipEdit::place (clips, makeClip (2, 1000, 1000, data), ids);
            expectEquals ((int) clips.size(), 3);
            expect (clips[0].id == 1 && clips[0].end() == 1000);
            expect (clips[1].id == 2 && clips[1].start == 1000);
            expect (clips[2].start == 2000 && clips[2].end() == 3000 && clips[2].offset == 2000);
        }

        beginTest ("trims stay inside the source and keep the audio in place");
        {
            auto clip = makeClip (1, 1000, 2000, data, 500);
            ClipEdit::trimStart (clip, 0);   // only 500 samples of source before
            expect (clip.start == 500 && clip.offset == 0 && clip.length == 2500);
            ClipEdit::trimStart (clip, 10000);   // at most up to one sample before the end
            expect (clip.end() == 3000 && clip.length == 1);
            ClipEdit::trimStart (clip, 1500);
            expect (clip.start == 1500 && clip.offset == 1000 && clip.length == 1500);

            ClipEdit::trimEnd (clip, 100000);    // source ends at 10000 samples
            expect (clip.offset + clip.length == 10000);
            ClipEdit::trimEnd (clip, 0);
            expectEquals ((int) clip.length, 1);
        }

        beginTest ("fades are clamped into the clip");
        {
            auto clip = makeClip (1, 0, 100, data);
            clip.fadeIn = 80;
            clip.fadeOut = 80;
            ClipEdit::clampFades (clip);
            expect (clip.fadeIn == 80 && clip.fadeOut == 20);
        }

        beginTest ("waveform overview matches the samples");
        {
            const auto range = data->getRange (0, 10000);
            expectWithinAbsoluteError (range.first, 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (range.second, 9999.0f / 10000.0f, 1.0e-6f);
            const auto part = data->getRange (300, 700);   // not block aligned
            expectWithinAbsoluteError (part.first, 300.0f / 10000.0f, 1.0e-6f);
            expectWithinAbsoluteError (part.second, 699.0f / 10000.0f, 1.0e-6f);
        }
    }
};

static ClipEditTests clipEditTests;
