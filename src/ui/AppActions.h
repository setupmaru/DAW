#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// What the edit and mix windows ask the main component to do: things that
// need the plugin list, editor windows, file choosers or the transport.
// Implemented by MainComponent. Message thread only.
class AppActions
{
public:
    virtual ~AppActions() = default;

    static constexpr int kMasterId = 0;   // "track id" of the master bus

    // The menu of an insert slot: the plugin list for an empty slot
    // (entryId < 0), or edit/bypass/move/remove for a used one.
    virtual void showInsertMenu (int trackId, int entryId, juce::Component& anchor) = 0;
    virtual void openPluginEditor (int trackId, int entryId) = 0;

    // Imports files at a timeline position; trackId 0 makes new tracks.
    virtual void importFiles (const juce::StringArray& files, int trackId, juce::int64 position) = 0;

    virtual void locate (juce::int64 position) = 0;
    virtual void showTrackMenu (int trackId) = 0;
    virtual void showMessage (const juce::String&) = 0;
};

// A transport-style button that draws its symbol as a path (no font glyphs).
class IconButton : public juce::Button
{
public:
    enum class Icon { toStart, stop, play, record, loop, click, zoomIn, zoomOut };

    IconButton (const juce::String& name, Icon i, juce::Colour onColour = juce::Colour (0xff4f8fd6))
        : juce::Button (name), icon (i), activeColour (onColour)
    {
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();
        g.setColour (on ? activeColour.withAlpha (0.35f) : juce::Colour (0xff353942).brighter (over ? 0.15f : 0.0f).darker (down ? 0.2f : 0.0f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colour (0xff15171a));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const auto c = r.getCentre();
        const float s = std::min (r.getWidth(), r.getHeight()) * 0.28f;
        juce::Path p;
        auto colour = on ? activeColour.brighter (0.3f) : juce::Colour (0xffd8dbe0);

        switch (icon)
        {
            case Icon::toStart:
                p.addRectangle (c.x - s, c.y - s, s * 0.3f, s * 2.0f);
                p.addTriangle (c.x + s, c.y - s, c.x + s, c.y + s, c.x - s * 0.6f, c.y);
                break;
            case Icon::stop:
                p.addRectangle (c.x - s * 0.85f, c.y - s * 0.85f, s * 1.7f, s * 1.7f);
                break;
            case Icon::play:
                p.addTriangle (c.x - s * 0.8f, c.y - s, c.x - s * 0.8f, c.y + s, c.x + s, c.y);
                if (on) colour = juce::Colour (0xff5fd37a);
                break;
            case Icon::record:
                p.addEllipse (c.x - s, c.y - s, s * 2.0f, s * 2.0f);
                colour = on ? juce::Colour (0xffff5a4f) : juce::Colour (0xffc0504a);
                break;
            case Icon::loop:
            {
                juce::Path arc;
                arc.addCentredArc (c.x, c.y, s, s * 0.8f, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.2f, true);
                g.setColour (colour);
                g.strokePath (arc, juce::PathStrokeType (2.0f));
                p.addTriangle (c.x + s * 0.2f, c.y - s * 1.25f, c.x + s * 0.2f, c.y - s * 0.35f, c.x + s * 0.9f, c.y - s * 0.8f);
                break;
            }
            case Icon::click:
                p.addTriangle (c.x - s * 0.8f, c.y + s, c.x + s * 0.8f, c.y + s, c.x, c.y - s);
                p.addRectangle (c.x - s * 0.08f, c.y - s * 0.6f, s * 1.0f, s * 0.18f);
                break;
            case Icon::zoomIn:
            case Icon::zoomOut:
                p.addRectangle (c.x - s, c.y - s * 0.12f, s * 2.0f, s * 0.24f);
                if (icon == Icon::zoomIn)
                    p.addRectangle (c.x - s * 0.12f, c.y - s, s * 0.24f, s * 2.0f);
                break;
        }

        g.setColour (colour);
        g.fillPath (p);
    }

private:
    Icon icon;
    juce::Colour activeColour;
};
