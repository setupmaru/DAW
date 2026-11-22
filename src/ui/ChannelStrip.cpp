#include "ChannelStrip.h"

#include "DAWLookAndFeel.h"
#include "EditWindow.h"

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }
constexpr int kSlotHeight = 19;

juce::String formatDb (double db)
{
    if (db <= -60.0)
        return "-inf";
    return (db > 0.0 ? "+" : "") + juce::String (db, 1);
}
} // namespace

ChannelStrip::ChannelStrip (Session& s, AppActions& a, int id)
    : session (s), actions (a), trackId (id)
{
    if (! isMaster())
        audio = session.findTrack (trackId)->getAudioPtr();
    auto& source = isMaster() ? session.getEngine().getMaster() : *audio;
    meter = std::make_unique<MeterBar> (source.meterL, source.meterR, true);
    addAndMakeVisible (*meter);

    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader.setRange (-60.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setWantsKeyboardFocus (false);
    fader.setTooltip (utf8 ("볼륨 (두 번 누르면 0 dB)"));
    fader.onValueChange = [this]
    {
        const double v = fader.getValue();
        const float db = v <= -60.0 ? TrackAudio::kMinDb : (float) v;
        if (isMaster())
            session.setMasterVolume (db);
        else
            session.setTrackVolume (trackId, db);
        dbLabel.setText (formatDb (v), juce::dontSendNotification);
    };
    addAndMakeVisible (fader);

    dbLabel.setJustificationType (juce::Justification::centred);
    dbLabel.setFont (juce::FontOptions (12.0f));
    dbLabel.setColour (juce::Label::backgroundColourId, DAWColours::background);
    addAndMakeVisible (dbLabel);

    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);

    if (! isMaster())
    {
        pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setWantsKeyboardFocus (false);
        pan.setTooltip (utf8 ("팬: 왼쪽/오른쪽 (두 번 누르면 가운데)"));
        pan.onValueChange = [this] { session.setTrackPan (trackId, (float) pan.getValue()); };
        addAndMakeVisible (pan);

        inputBox.setWantsKeyboardFocus (false);
        inputBox.onChange = [this]
        {
            const int sel = inputBox.getSelectedId();
            if (sel > 0)
                session.setTrackInput (trackId, sel > 1000 ? sel - 1001 : sel - 1, sel > 1000);
        };
        addAndMakeVisible (inputBox);

        auto setup = [this] (juce::TextButton& b, juce::Colour on, const juce::String& tip)
        {
            b.setColour (juce::TextButton::buttonOnColourId, on);
            b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            b.setWantsKeyboardFocus (false);
            b.setTooltip (tip);
            addAndMakeVisible (b);
        };
        setup (recButton, DAWColours::record, utf8 ("녹음 준비"));
        setup (soloButton, DAWColours::solo, utf8 ("솔로"));
        setup (muteButton, DAWColours::mute, utf8 ("음소거"));
        recButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackArmed (trackId, ! t->armed); };
        soloButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackSolo (trackId, ! t->solo); };
        muteButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackMute (trackId, ! t->mute); };
    }

    refresh();
}

PluginChain* ChannelStrip::getInserts() const
{
    if (isMaster())
        return &session.getMasterInserts();
    auto* track = session.findTrack (trackId);
    return track != nullptr ? &track->getInserts() : nullptr;
}

void ChannelStrip::refresh()
{
    float db = 0.0f;

    if (isMaster())
    {
        db = session.getMasterVolume();
        nameLabel.setText (utf8 ("마스터"), juce::dontSendNotification);
    }
    else if (auto* track = session.findTrack (trackId))
    {
        db = track->volumeDb;
        nameLabel.setText (track->name, juce::dontSendNotification);
        nameLabel.setColour (juce::Label::backgroundColourId, track->colour.withMultipliedBrightness (0.6f));
        if (! pan.isMouseButtonDown())
            pan.setValue (track->pan, juce::dontSendNotification);
        recButton.setToggleState (track->armed, juce::dontSendNotification);
        soloButton.setToggleState (track->solo, juce::dontSendNotification);
        muteButton.setToggleState (track->mute, juce::dontSendNotification);

        int inputs = 2;
        if (auto* device = session.getEngine().getDeviceManager().getCurrentAudioDevice())
            inputs = std::max (1, device->getActiveInputChannels().countNumberOfSetBits());
        if (inputs != shownInputs)
        {
            TrackHeader::fillInputMenu (inputBox, std::max (inputs, track->inputChannel + (track->stereoInput ? 2 : 1)));
            shownInputs = inputs;
        }
        inputBox.setSelectedId (TrackHeader::inputId (track->inputChannel, track->stereoInput), juce::dontSendNotification);
    }

    if (! fader.isMouseButtonDown())
        fader.setValue (std::max (-60.0f, db), juce::dontSendNotification);
    dbLabel.setText (formatDb (fader.getValue()), juce::dontSendNotification);
    repaint (insertArea);
}

juce::Rectangle<int> ChannelStrip::slotBounds (int index) const
{
    return { insertArea.getX(), insertArea.getY() + 16 + index * kSlotHeight, insertArea.getWidth(), kSlotHeight - 2 };
}

void ChannelStrip::paint (juce::Graphics& g)
{
    g.fillAll (isMaster() ? DAWColours::panelLight : DAWColours::panel);
    g.setColour (DAWColours::outline);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

    g.setColour (DAWColours::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (utf8 ("인서트"), insertArea.withHeight (14), juce::Justification::centredLeft);

    const auto* inserts = getInserts();
    const auto& entries = inserts != nullptr ? inserts->getEntries() : std::vector<ChainEntry>();

    for (int i = 0; i < kNumInsertSlots; ++i)
    {
        const auto r = slotBounds (i).toFloat();
        const bool used = i < (int) entries.size();
        g.setColour (used ? (entries[(size_t) i].bypassed ? juce::Colour (0xff4a4d55) : juce::Colour (0xff3d6fa8)) : DAWColours::background);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (DAWColours::outline);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);

        if (used)
        {
            g.setColour (entries[(size_t) i].bypassed ? DAWColours::textDim : juce::Colours::white);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (entries[(size_t) i].name, r.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }
        else if (i == (int) entries.size())
        {
            g.setColour (DAWColours::textDim.withAlpha (0.6f));
            g.setFont (juce::FontOptions (12.0f));
            g.drawText ("+", r.toNearestInt(), juce::Justification::centred);
        }
    }

    if ((int) entries.size() > kNumInsertSlots)
    {
        g.setColour (DAWColours::textDim);
        g.drawText ("+" + juce::String ((int) entries.size() - kNumInsertSlots), slotBounds (kNumInsertSlots), juce::Justification::centredRight);
    }
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    const auto* inserts = getInserts();
    if (inserts == nullptr)
        return;

    for (int i = 0; i < kNumInsertSlots; ++i)
    {
        if (! slotBounds (i).contains (e.getPosition()))
            continue;

        const auto& entries = inserts->getEntries();
        const int entryId = i < (int) entries.size() ? entries[(size_t) i].id : -1;

        // Double-click on a plugin opens its window; a click shows the menu.
        if (entryId >= 0 && e.getNumberOfClicks() > 1)
            actions.openPluginEditor (trackId, entryId);
        else
            actions.showInsertMenu (trackId, entryId, *this);
        return;
    }

    if (! isMaster() && e.mods.isPopupMenu())
        actions.showTrackMenu (trackId);
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (5, 6);
    nameLabel.setBounds (r.removeFromBottom (24));
    r.removeFromBottom (4);
    dbLabel.setBounds (r.removeFromBottom (18).reduced (14, 0));
    r.removeFromBottom (4);

    insertArea = r.removeFromTop (16 + (kNumInsertSlots + 1) * kSlotHeight);

    if (! isMaster())
    {
        r.removeFromTop (4);
        inputBox.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        pan.setBounds (r.removeFromTop (44).withSizeKeepingCentre (44, 44));
        r.removeFromTop (4);
        auto buttons = r.removeFromTop (22);
        const int w = buttons.getWidth() / 3;
        recButton.setBounds (buttons.removeFromLeft (w).reduced (1));
        soloButton.setBounds (buttons.removeFromLeft (w).reduced (1));
        muteButton.setBounds (buttons.reduced (1));
    }
    else
    {
        r.removeFromTop (4 + 22 + 4 + 44 + 4 + 22);   // line the fader up with the tracks'
    }

    r.removeFromTop (8);
    meter->setBounds (r.removeFromRight (18));
    r.removeFromRight (4);
    fader.setBounds (r);
}
