#include "EditState.h"

#include <cmath>

juce::StringArray EditState::gridNames()
{
    return { juce::String (juce::CharPointer_UTF8 ("1마디")), "1/2", "1/4", "1/8", "1/16" };
}

double EditState::gridSamples() const noexcept
{
    static const double fractions[kNumGrids] = { 1.0, 0.5, 0.25, 0.125, 0.0625 };
    return barSamples() * fractions[juce::jlimit (0, kNumGrids - 1, gridIndex)];
}

juce::int64 EditState::snap (juce::int64 t) const noexcept
{
    if (mode != EditMode::grid)
        return t;

    const double g = gridSamples();
    return (juce::int64) std::llround (std::round ((double) t / g) * g);
}

void EditState::zoom (double factor, float anchorX)
{
    const double anchor = scrollSamples + (double) anchorX * samplesPerPixel;
    samplesPerPixel = juce::jlimit (kMinSamplesPerPixel, kMaxSamplesPerPixel, samplesPerPixel * factor);
    setScroll (anchor - (double) anchorX * samplesPerPixel);
}

void EditState::setScroll (double samples)
{
    scrollSamples = std::max (0.0, samples);
    changed();
}

void EditState::clearSelection()
{
    selectedClips.clear();
    selection = {};
    changed();
}

juce::String EditState::formatMinSec (juce::int64 t) const
{
    const double seconds = (double) std::max<juce::int64> (0, t) / sampleRate;
    const int minutes = (int) (seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    return juce::String (minutes) + ":" + juce::String (rest, 3).paddedLeft ('0', 6);
}

juce::String EditState::formatBars (juce::int64 t) const
{
    const double beats = (double) std::max<juce::int64> (0, t) / beatSamples();
    const auto wholeBeats = (juce::int64) std::floor (beats);
    const int ticks = (int) std::floor ((beats - (double) wholeBeats) * 960.0);
    const auto bar = wholeBeats / 4 + 1;
    const int beat = (int) (wholeBeats % 4) + 1;
    return juce::String (bar) + "|" + juce::String (beat) + "|" + juce::String (ticks).paddedLeft ('0', 3);
}
