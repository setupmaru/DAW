#include "MeterBar.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
constexpr int kTimerHz = 30;
constexpr float kDecay = 0.86f;
constexpr float kMinDb = -60.0f;
constexpr int kHoldTicks = 45;   // 1.5 s

float proportion (float gain)
{
    const float db = juce::Decibels::gainToDecibels (gain, kMinDb);
    return juce::jlimit (0.0f, 1.0f, (db - kMinDb) / -kMinDb);
}
} // namespace

MeterBar::MeterBar (LevelMeterSource& l, LevelMeterSource& r, bool isVertical)
    : sourceL (l), sourceR (r), vertical (isVertical)
{
    setInterceptsMouseClicks (true, false);
    startTimerHz (kTimerHz);
}

MeterBar::~MeterBar()
{
    stopTimer();
}

void MeterBar::timerCallback()
{
    const float peaks[2] = { sourceL.readAndReset(), sourceR.readAndReset() };
    bool changedAny = false;

    for (int ch = 0; ch < 2; ++ch)
    {
        const float before = level[ch];
        level[ch] = std::max (peaks[ch], level[ch] * kDecay);
        if (level[ch] < 1.0e-4f)
            level[ch] = 0.0f;

        if (peaks[ch] >= hold[ch])
        {
            hold[ch] = peaks[ch];
            holdTicks[ch] = kHoldTicks;
        }
        else if (--holdTicks[ch] <= 0)
        {
            hold[ch] = level[ch];
        }

        if (peaks[ch] >= 1.0f)
            clipped = true;

        changedAny = changedAny || ! juce::approximatelyEqual (before, level[ch]);
    }

    if (changedAny)
        repaint();
}

void MeterBar::mouseDown (const juce::MouseEvent&)
{
    clipped = false;
    repaint();
}

void MeterBar::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff111214));
    g.fillRect (area);

    const float clipSize = vertical ? 5.0f : 5.0f;
    auto clipArea = vertical ? area.removeFromTop (clipSize) : area.removeFromRight (clipSize);
    g.setColour (clipped ? juce::Colour (0xffff3b30) : juce::Colour (0xff3a1c1a));
    g.fillRect (clipArea.reduced (0.5f));

    for (int ch = 0; ch < 2; ++ch)
    {
        auto bar = vertical ? area.withWidth (area.getWidth() / 2.0f).withX (area.getX() + (float) ch * area.getWidth() / 2.0f).reduced (1.0f, 0.5f)
                            : area.withHeight (area.getHeight() / 2.0f).withY (area.getY() + (float) ch * area.getHeight() / 2.0f).reduced (0.5f, 1.0f);

        const float p = proportion (level[ch]);
        juce::ColourGradient gradient = vertical
            ? juce::ColourGradient (juce::Colour (0xffff4a3d), 0.0f, bar.getY(), juce::Colour (0xff35c45a), 0.0f, bar.getBottom(), false)
            : juce::ColourGradient (juce::Colour (0xff35c45a), bar.getX(), 0.0f, juce::Colour (0xffff4a3d), bar.getRight(), 0.0f, false);
        gradient.addColour (0.12, juce::Colour (0xffffc83d));
        gradient.addColour (0.35, juce::Colour (0xff8fd14f));
        g.setGradientFill (gradient);

        if (vertical)
            g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * p));
        else
            g.fillRect (bar.withWidth (bar.getWidth() * p));

        const float h = proportion (hold[ch]);
        if (h > 0.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            if (vertical)
                g.fillRect (bar.getX(), bar.getBottom() - bar.getHeight() * h, bar.getWidth(), 1.5f);
            else
                g.fillRect (bar.getX() + bar.getWidth() * h - 1.5f, bar.getY(), 1.5f, bar.getHeight());
        }
    }
}
