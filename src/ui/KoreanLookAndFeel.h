#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The app's look and feel: JUCE's default V4 look with an OS-specific Korean
// font as the default sans-serif typeface, so Hangul renders everywhere.
// Install it with LookAndFeel::setDefaultLookAndFeel() before any window is
// created, and reset the default to nullptr before this object is destroyed.
class KoreanLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KoreanLookAndFeel()
    {
        setDefaultSansSerifTypefaceName (getKoreanFontName());
    }

    static juce::String getKoreanFontName()
    {
       #if JUCE_WINDOWS
        return "Malgun Gothic";
       #elif JUCE_MAC
        return "Apple SD Gothic Neo";
       #else   // JUCE_LINUX (and other Unix-likes)
        return "Noto Sans CJK KR";
       #endif
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KoreanLookAndFeel)
};
