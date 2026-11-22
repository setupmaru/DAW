#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "audio/LevelMeterSource.h"

// A stereo peak meter (two bars), vertical or horizontal, -60..0 dBFS with a
// peak-hold line and a red clip light that stays until clicked.
// A 30 Hz timer reads the sources with readAndReset().
//
// Lifetime: the sources must outlive this component.
class MeterBar : public juce::Component,
                 private juce::Timer
{
public:
    MeterBar (LevelMeterSource& left, LevelMeterSource& right, bool vertical = true);
    ~MeterBar() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;   // resets the clip light

private:
    void timerCallback() override;

    LevelMeterSource& sourceL;
    LevelMeterSource& sourceR;
    const bool vertical;
    float level[2] {}, hold[2] {};
    int holdTicks[2] {};
    bool clipped = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterBar)
};
