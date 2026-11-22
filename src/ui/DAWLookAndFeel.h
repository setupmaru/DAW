#pragma once

#include "KoreanLookAndFeel.h"

// Colours of the app: a dark, low-contrast studio look (grey panels, blue
// selection, red record, yellow playhead), with the Korean default font.
namespace DAWColours
{
    inline const juce::Colour background   { 0xff1e2024 };
    inline const juce::Colour panel        { 0xff2a2d33 };
    inline const juce::Colour panelLight   { 0xff353942 };
    inline const juce::Colour laneA        { 0xff24272c };
    inline const juce::Colour laneB        { 0xff272a30 };
    inline const juce::Colour outline      { 0xff15171a };
    inline const juce::Colour text         { 0xffd8dbe0 };
    inline const juce::Colour textDim      { 0xff8a909a };
    inline const juce::Colour accent       { 0xff4f8fd6 };
    inline const juce::Colour selection    { 0x554f8fd6 };
    inline const juce::Colour playhead     { 0xffffd34d };
    inline const juce::Colour record       { 0xffe0463c };
    inline const juce::Colour solo         { 0xffe6c53c };
    inline const juce::Colour mute         { 0xffe08a3c };
    inline const juce::Colour input        { 0xff5fd37a };
    inline const juce::Colour grid         { 0xff30343b };
    inline const juce::Colour gridBar      { 0xff3c414a };
}

class DAWLookAndFeel : public KoreanLookAndFeel
{
public:
    DAWLookAndFeel()
    {
        using namespace DAWColours;
        setColourScheme ({ panel, background, panelLight, textDim, text, accent, juce::Colours::white, accent, text });

        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::TextButton::buttonColourId, panelLight);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ComboBox::backgroundColourId, panelLight);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::Label::textColourId, text);
        setColour (juce::Slider::thumbColourId, juce::Colour (0xffc9ced6));
        setColour (juce::Slider::trackColourId, panelLight);
        setColour (juce::Slider::backgroundColourId, outline);
        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, outline);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::ScrollBar::thumbColourId, panelLight.brighter (0.2f));
        setColour (juce::TooltipWindow::backgroundColourId, panelLight);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TextEditor::backgroundColourId, background);
        setColour (juce::TextEditor::outlineColourId, outline);
        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId, text);
    }

    // A fader cap like a console's: a wide grey cap with a centre line.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            KoreanLookAndFeel::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const float cx = (float) x + (float) width * 0.5f;
        g.setColour (DAWColours::outline);
        g.fillRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) height, 2.0f);

        const float capW = std::min (34.0f, (float) width - 4.0f), capH = 22.0f;
        juce::Rectangle<float> cap (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffb9bec6), cap.getX(), cap.getY(),
                                                 juce::Colour (0xff6f757e), cap.getX(), cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (DAWColours::outline);
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawHorizontalLine ((int) sliderPos, cap.getX() + 3.0f, cap.getRight() - 3.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions ((float) std::min (14, buttonHeight - 6)));
    }
};
