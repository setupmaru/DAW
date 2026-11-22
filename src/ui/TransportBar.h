#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AppActions.h"
#include "EditState.h"
#include "session/Session.h"

// The bar across the top of the window:
//   edit mode + grid | tools | transport | counter | selection | tempo | zoom | edit/mix
// Every button invokes an application command, so buttons, menus and
// shortcuts behave the same. A timer keeps the counter and states current.
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar (Session&, EditState&, juce::ApplicationCommandManager&);
    ~TransportBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;   // the counter toggles bars / min:sec

    void setMixShown (bool);

private:
    void timerCallback() override;
    void refresh();
    juce::TextButton& makeTextButton (juce::TextButton&, const juce::String& text, int commandId, const juce::String& tip);
    void makeIconButton (IconButton&, int commandId, const juce::String& tip);

    Session& session;
    EditState& state;
    juce::ApplicationCommandManager& commands;

    juce::TextButton slipButton, gridButton, trimButton, selectButton, grabButton, editButton, mixButton;
    juce::ComboBox gridBox;
    IconButton toStartButton { "toStart", IconButton::Icon::toStart },
               stopButton { "stop", IconButton::Icon::stop },
               playButton { "play", IconButton::Icon::play, juce::Colour (0xff5fd37a) },
               recordButton { "record", IconButton::Icon::record, juce::Colour (0xffe0463c) },
               loopButton { "loop", IconButton::Icon::loop },
               clickButton { "click", IconButton::Icon::click },
               zoomOutButton { "zoomOut", IconButton::Icon::zoomOut },
               zoomInButton { "zoomIn", IconButton::Icon::zoomIn };
    juce::Label tempoLabel, tempoCaption;
    juce::Rectangle<int> counterArea, selectionArea;
    juce::String mainCounter, subCounter, selectionText;
    bool recordBlink = false;
    int blinkTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
