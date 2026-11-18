#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "audio/LevelMeterSource.h"

// A horizontal peak meter with a text label ("입력" / "출력") on its left.
// A 30 Hz timer reads the source with readAndReset(); the shown level falls
// off smoothly (decay) instead of jumping to zero. Segments near 0 dBFS are
// red, the ones just below yellow, the rest green.
//
// Lifetime: the LevelMeterSource must outlive this component.
class LevelMeter : public juce::Component,
                   private juce::Timer
{
public:
    LevelMeter (LevelMeterSource& source, const juce::String& label);
    ~LevelMeter() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    int litSegments() const;

    LevelMeterSource& source;
    const juce::String label;
    float level = 0.0f;       // linear peak, with decay applied
    int lastLitSegments = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
