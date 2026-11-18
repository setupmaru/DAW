#include "LevelMeter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace
{
constexpr int kTimerHz = 30;
constexpr float kDecayPerTick = 0.85f;   // about -42 dB per second at 30 Hz
constexpr float kMinDb = -60.0f;
constexpr int kNumSegments = 20;         // 3 dB per segment
constexpr float kRedFromDb = -3.0f;      // near 0 dBFS
constexpr float kYellowFromDb = -12.0f;
constexpr int kLabelWidth = 40;

float segmentLowerDb (int index)
{
    return kMinDb + (float) index * (-kMinDb / (float) kNumSegments);
}
} // namespace

LevelMeter::LevelMeter (LevelMeterSource& s, const juce::String& text)
    : source (s), label (text)
{
    startTimerHz (kTimerHz);
}

LevelMeter::~LevelMeter()
{
    stopTimer();
}

int LevelMeter::litSegments() const
{
    const float db = juce::Decibels::gainToDecibels (level, kMinDb - 1.0f);
    int lit = 0;

    for (int i = 0; i < kNumSegments; ++i)
        if (db >= segmentLowerDb (i))
            lit = i + 1;

    return lit;
}

void LevelMeter::timerCallback()
{
    const float peak = source.readAndReset();
    level = std::max (peak, level * kDecayPerTick);

    if (level < 1.0e-5f)
        level = 0.0f;

    const int lit = litSegments();

    if (lit != lastLitSegments)
    {
        lastLitSegments = lit;
        repaint();
    }
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (findColour (juce::Label::textColourId));
    g.setFont (juce::FontOptions (15.0f));
    g.drawText (label, area.removeFromLeft (kLabelWidth), juce::Justification::centredLeft);

    auto bar = area.reduced (0, 6).toFloat();
    const float gap = 2.0f;
    const float segmentWidth = (bar.getWidth() - gap * (float) (kNumSegments - 1)) / (float) kNumSegments;

    if (segmentWidth <= 0.0f)
        return;

    const int lit = litSegments();

    for (int i = 0; i < kNumSegments; ++i)
    {
        const float lowerDb = segmentLowerDb (i);
        auto colour = lowerDb >= kRedFromDb      ? juce::Colours::red
                    : lowerDb >= kYellowFromDb   ? juce::Colours::yellow
                                                 : juce::Colours::limegreen;

        if (i >= lit)
            colour = colour.withAlpha (0.15f);

        g.setColour (colour);
        g.fillRect (bar.getX() + (float) i * (segmentWidth + gap), bar.getY(), segmentWidth, bar.getHeight());
    }
}
