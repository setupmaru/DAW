#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AppActions.h"
#include "EditState.h"
#include "session/Session.h"

// The ruler (bars|beats and min:sec) above the track lanes, with clips,
// waveforms, fades, the time selection, the edit cursor and the playhead.
//
// Mouse, by tool (EditState::tool):
//  - Grab:   click selects a clip (Shift adds), drag moves the selection across
//            time and tracks (Alt: copies). Clip edges trim, the small squares
//            at a clip's top corners set the fades.
//  - Trim:   drag anywhere on a clip to move its nearer edge.
//  - Select: drag selects a time range across tracks; click places the cursor;
//            double-click selects a clip's range.
//  - Ruler:  click locates the playhead.
//  Wheel scrolls tracks, Shift+wheel scrolls time, Ctrl+wheel zooms.
// Drop audio files onto a track (or below the last one for new tracks).
//
// A drag edits the clips live without undo entries and commits one undo step
// when the mouse is released.
class TimelineView : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    TimelineView (Session&, EditState&, AppActions&);
    ~TimelineView() override;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    int getLanesHeight() const;   // all tracks, for the vertical scroll range
    bool isDragging() const noexcept { return drag.kind != DragKind::none; }

    std::function<void()> onScrollChanged;   // the wheel or auto-scroll moved the view

private:
    enum class Zone { none, body, trimStart, trimEnd, fadeIn, fadeOut };
    enum class DragKind { none, move, trimStart, trimEnd, fadeIn, fadeOut, select, ruler };

    struct Hit
    {
        int trackIndex = -1;
        int clipId = 0;
        Zone zone = Zone::none;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    int trackIndexAt (int y) const;           // -1: none (ruler or below the last track)
    int laneTop (int trackIndex) const;
    Hit hitClip (juce::Point<int>) const;
    juce::Rectangle<float> clipBounds (const Clip&, int trackIndex) const;

    void paintRuler (juce::Graphics&);
    void paintGrid (juce::Graphics&, juce::Rectangle<int> lanes);
    void paintClip (juce::Graphics&, const Clip&, const Track&, int trackIndex);
    void applyDrag (const juce::MouseEvent&);

    Session& session;
    EditState& state;
    AppActions& actions;

    struct Drag
    {
        DragKind kind = DragKind::none;
        Session::ClipMap before;
        int clipId = 0;                         // the clip under the mouse
        int downTrackIndex = 0;
        juce::int64 downTime = 0;
        juce::int64 anchorStart = 0;            // the clip's start when grabbed
        std::vector<std::pair<int, int>> moving;   // clip id, track id
        std::map<int, int> copyIds;             // original -> copy (Alt-drag)
        bool copy = false;
        bool changed = false;
    } drag;

    juce::int64 lastPlayhead = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
