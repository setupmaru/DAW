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
       #else   // JUCE_LINUX (and other Unix-likes): the first installed font with Hangul
        const auto installed = juce::Font::findAllTypefaceNames();
        // Unifont covers Hangul but looks rough; DejaVu at least shows Latin text.
        for (const auto* name : { "Noto Sans CJK KR", "Noto Sans KR", "NanumGothic", "UnDotum", "Unifont", "DejaVu Sans" })
            if (installed.contains (name))
                return name;
        return installed.isEmpty() ? juce::String ("Noto Sans CJK KR") : installed[0];
       #endif
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KoreanLookAndFeel)
};
