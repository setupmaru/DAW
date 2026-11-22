#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AppActions.h"
#include "EditState.h"
#include "MeterBar.h"
#include "TimelineView.h"
#include "session/Session.h"

#include <map>
#include <memory>

// One row of the track list on the left of the edit window:
//   colour | name            [R] [S] [M]
//          | input ▼   volume ────●──   meter
class TrackHeader : public juce::Component
{
public:
    TrackHeader (Session&, EditState&, AppActions&, Track&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    void refresh();          // copies the track's state into the controls
    int getTrackId() const noexcept { return trackId; }

    static void fillInputMenu (juce::ComboBox&, int numInputs);
    static int inputId (int channel, bool stereo) { return stereo ? 1000 + channel + 1 : channel + 1; }

private:
    Session& session;
    EditState& state;
    AppActions& actions;
    const int trackId;
    // Keeps the meter sources alive until this header is gone, even when the
    // track was removed and the engine freed its last snapshot first.
    std::shared_ptr<TrackAudio> audio;

    juce::Label nameLabel;
    juce::TextButton recButton { "R" }, soloButton { "S" }, muteButton { "M" };
    juce::ComboBox inputBox;
    juce::Slider volume;
    MeterBar meter;
    int shownInputs = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeader)
};

// The edit window: track headers on the left, the timeline on the right,
// with horizontal and vertical scroll bars.
class EditWindow : public juce::Component,
                   private juce::ChangeListener,
                   private juce::ScrollBar::Listener
{
public:
    EditWindow (Session&, EditState&, AppActions&);
    ~EditWindow() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    TimelineView& getTimeline() noexcept { return timeline; }
    int getTimelineWidth() const noexcept { return timeline.getWidth(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void syncHeaders();
    void layoutHeaders();
    void updateScrollBars();

    Session& session;
    EditState& state;
    AppActions& actions;

    juce::Component headerArea;   // clips the headers to the track area
    std::vector<std::unique_ptr<TrackHeader>> headers;
    TimelineView timeline;
    juce::ScrollBar vScroll { true }, hScroll { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditWindow)
};
