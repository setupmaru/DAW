#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/TimelineView.h"

namespace
{
constexpr double kRate = 48000.0;

struct NullActions : public AppActions
{
    void showInsertMenu (int, int, juce::Component&) override {}
    void openPluginEditor (int, int) override {}
    void importFiles (const juce::StringArray&, int, juce::int64) override {}
    void locate (juce::int64 p) override { located = p; }
    void showTrackMenu (int) override {}
    void showMessage (const juce::String&) override {}
    juce::int64 located = -1;
};

AudioDataPtr makeConstant (int length)
{
    juce::AudioBuffer<float> buffer (1, length);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), 0.5f, length);
    return makeAudioData (std::move (buffer), kRate);
}

// Drives a component with synthetic mouse events: down at `from`, drag to
// `to` (in steps), up.
void dragMouse (juce::Component& c, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
{
    auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    auto event = [&] (juce::Point<float> p, bool dragged, int clicks = 1)
    {
        return juce::MouseEvent (source, p, mods.withFlags (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &c, &c, now, from, now, clicks, dragged);
    };

    c.mouseDown (event (from, false));
    for (int i = 1; i <= 4; ++i)
        c.mouseDrag (event (from + (to - from) * ((float) i / 4.0f), true));
    c.mouseUp (event (to, from != to));
}
} // namespace

struct TimelineViewTests : public juce::UnitTest
{
    TimelineViewTests() : juce::UnitTest ("TimelineViewTests", "DAW") {}

    void runTest() override
    {
        MixEngine engine;
        juce::AudioPluginFormatManager plugins;
        juce::AudioFormatManager formats;
        Session session (engine, plugins, formats);
        auto* a = session.addTrack ("A");
        auto* b = session.addTrack ("B");

        EditState state;
        state.sampleRate = kRate;
        state.samplesPerPixel = 100.0;   // 1 px = 100 samples
        state.trackHeight = 100;
        NullActions actions;
        TimelineView view (session, state, actions);
        view.setSize (1200, EditState::kRulerHeight + 300);

        auto reset = [&]
        {
            Clip clip;
            clip.id = session.newClipId();
            clip.name = "c";
            clip.data = makeConstant (96000);
            clip.start = 48000;
            clip.length = 48000;   // x 480 .. 960
            clip.offset = 24000;
            Session::ClipMap map;
            map[a->id] = { clip };
            map[b->id] = {};
            session.applyClips (map);
            session.getUndoManager().clearUndoHistory();
            state.selectedClips.clear();
            state.selection = {};
            state.tool = EditTool::grab;
            state.mode = EditMode::slip;
            return clip.id;
        };
        const float laneA = (float) EditState::kRulerHeight + 50.0f;   // middle of track A
        const float topA = (float) EditState::kRulerHeight + 2.0f;

        beginTest ("grab tool drags a clip in time, as one undo step");
        {
            const int id = reset();
            dragMouse (view, { 600.0f, laneA }, { 700.0f, laneA });
            expectEquals ((int) a->clips.size(), 1);
            expectEquals ((int) a->clips[0].start, 58000);
            expect (state.selectedClips.count (id) == 1, "the dragged clip is selected");
            expect (session.getUndoManager().canUndo());
            session.getUndoManager().undo();
            expectEquals ((int) a->clips[0].start, 48000);
            expect (! session.getUndoManager().canUndo(), "one step for the whole drag");
        }

        beginTest ("grid mode snaps the moved clip to the grid");
        {
            reset();
            state.mode = EditMode::grid;
            state.gridIndex = 2;   // quarter note: 24000 samples at 120 BPM / 48 kHz
            dragMouse (view, { 600.0f, laneA }, { 713.0f, laneA });   // +11300: snaps to the nearest quarter
            expectEquals ((int) (a->clips[0].start % 24000), 0, "on a grid line");
            expectEquals ((int) a->clips[0].start, 48000 + 24000 * (int) std::round (11300.0 / 24000.0));
        }

        beginTest ("dragging down moves the clip to the next track");
        {
            reset();
            dragMouse (view, { 600.0f, laneA }, { 600.0f, laneA + 100.0f });
            expect (a->clips.empty());
            expectEquals ((int) b->clips.size(), 1);
            expectEquals ((int) b->clips[0].start, 48000);
        }

        beginTest ("Alt-drag copies instead of moving");
        {
            reset();
            dragMouse (view, { 600.0f, laneA }, { 1100.0f, laneA }, juce::ModifierKeys::altModifier);
            expectEquals ((int) a->clips.size(), 2);
            expectEquals ((int) a->clips[0].start, 48000);
            expectEquals ((int) a->clips[1].start, 98000);
            expect (a->clips[0].id != a->clips[1].id);
        }

        beginTest ("the right edge trims the clip end");
        {
            reset();
            dragMouse (view, { 958.0f, laneA }, { 858.0f, laneA });
            expectEquals ((int) a->clips[0].start, 48000);
            expectEquals ((int) a->clips[0].length, 38000);
        }

        beginTest ("the left edge trims the start but cannot pass the source start");
        {
            reset();
            dragMouse (view, { 482.0f, laneA }, { 100.0f, laneA });   // only 24000 samples of source before
            expectEquals ((int) a->clips[0].start, 24000);
            expectEquals ((int) a->clips[0].offset, 0);
            expectEquals ((int) a->clips[0].end(), 96000, "the end stays put");
        }

        beginTest ("the top-left square sets the fade-in");
        {
            reset();
            dragMouse (view, { 484.0f, topA + 4.0f }, { 534.0f, topA + 4.0f });
            expectEquals ((int) a->clips[0].fadeIn, 5000);
            expectEquals ((int) a->clips[0].start, 48000, "the clip did not move");
        }

        beginTest ("the select tool selects a time range across tracks and sets the cursor");
        {
            reset();
            state.tool = EditTool::select;
            dragMouse (view, { 100.0f, laneA }, { 300.0f, laneA + 100.0f });
            expectEquals ((int) state.selection.start, 10000);
            expectEquals ((int) state.selection.end, 30000);
            expect (state.selection.trackIds.count (a->id) == 1 && state.selection.trackIds.count (b->id) == 1);
            expectEquals ((int) state.cursor, 10000);
            expectEquals ((int) a->clips[0].start, 48000, "no clip moved");
        }

        beginTest ("clicking the ruler locates the playhead");
        {
            reset();
            dragMouse (view, { 250.0f, 10.0f }, { 250.0f, 10.0f });
            expectEquals ((int) actions.located, 25000);
        }

        session.applyClips ({});
    }
};

static TimelineViewTests timelineViewTests;
