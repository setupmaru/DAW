#include "MixWindow.h"

#include "DAWLookAndFeel.h"

#include <map>

MixWindow::MixWindow (Session& s, AppActions& a)
    : session (s), actions (a), masterStrip (s, a, AppActions::kMasterId)
{
    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
    addAndMakeVisible (masterStrip);
    session.addChangeListener (this);
    syncStrips();
}

MixWindow::~MixWindow()
{
    session.removeChangeListener (this);
}

void MixWindow::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncStrips();
    for (auto& strip : strips)
        strip->refresh();
    masterStrip.refresh();
}

void MixWindow::syncStrips()
{
    const auto& tracks = session.getTracks();
    bool same = tracks.size() == strips.size();
    for (size_t i = 0; same && i < tracks.size(); ++i)
        same = strips[i]->getTrackId() == tracks[i]->id;

    if (same)
        return;

    std::map<int, std::unique_ptr<ChannelStrip>> old;
    for (auto& strip : strips)
        old[strip->getTrackId()] = std::move (strip);
    strips.clear();

    for (const auto& track : tracks)
    {
        auto it = old.find (track->id);
        if (it != old.end())
            strips.push_back (std::move (it->second));
        else
        {
            strips.push_back (std::make_unique<ChannelStrip> (session, actions, track->id));
            stripHolder.addAndMakeVisible (*strips.back());
        }
    }

    resized();
}

void MixWindow::paint (juce::Graphics& g)
{
    g.fillAll (DAWColours::background);
}

void MixWindow::resized()
{
    auto r = getLocalBounds();
    masterStrip.setBounds (r.removeFromRight (ChannelStrip::kWidth + 8).withTrimmedLeft (8));
    viewport.setBounds (r);

    const int height = std::max (200, r.getHeight() - viewport.getScrollBarThickness());
    stripHolder.setSize ((int) strips.size() * ChannelStrip::kWidth, height);
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds ((int) i * ChannelStrip::kWidth, 0, ChannelStrip::kWidth, height);
}
