#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChannelStrip.h"

#include <memory>
#include <vector>

// The mix window: one channel strip per track, scrolling horizontally, and
// the master strip fixed on the right.
class MixWindow : public juce::Component,
                  private juce::ChangeListener
{
public:
    MixWindow (Session&, AppActions&);
    ~MixWindow() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void syncStrips();

    Session& session;
    AppActions& actions;
    juce::Viewport viewport;
    juce::Component stripHolder;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    ChannelStrip masterStrip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixWindow)
};
