#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AppActions.h"
#include "MeterBar.h"
#include "session/Session.h"

// One mixer channel (a track, or the master bus when trackId is
// AppActions::kMasterId):
//
//   inserts (8 slots; click an empty one to add a plugin)
//   input ▼
//   pan knob
//   [R] [I] [S] [M]
//   fader | meter
//   dB value
//   name
class ChannelStrip : public juce::Component
{
public:
    static constexpr int kWidth = 104;
    static constexpr int kNumInsertSlots = 8;

    ChannelStrip (Session&, AppActions&, int trackId);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    void refresh();
    int getTrackId() const noexcept { return trackId; }

private:
    bool isMaster() const noexcept { return trackId == AppActions::kMasterId; }
    PluginChain* getInserts() const;
    juce::Rectangle<int> slotBounds (int index) const;

    Session& session;
    AppActions& actions;
    const int trackId;
    std::shared_ptr<TrackAudio> audio;   // keeps the meter sources alive (null for the master)

    juce::ComboBox inputBox;
    juce::Slider pan, fader;
    juce::TextButton recButton { "R" }, inputButton { "I" }, soloButton { "S" }, muteButton { "M" };
    juce::Label dbLabel, nameLabel;
    std::unique_ptr<MeterBar> meter;
    int shownInputs = -1;
    juce::Rectangle<int> insertArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
