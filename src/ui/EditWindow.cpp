#include "EditWindow.h"

#include "DAWLookAndFeel.h"

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }
constexpr int kScrollBarSize = 14;

void styleToggle (juce::TextButton& b, juce::Colour onColour, const juce::String& tip)
{
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    b.setWantsKeyboardFocus (false);
    b.setTooltip (tip);
}

int numDeviceInputs (Session& session)
{
    if (auto* device = session.getEngine().getDeviceManager().getCurrentAudioDevice())
        return std::max (1, device->getActiveInputChannels().countNumberOfSetBits());
    return 2;
}
} // namespace

//==============================================================================
TrackHeader::TrackHeader (Session& s, EditState& e, AppActions& a, Track& track)
    : session (s), state (e), actions (a), trackId (track.id), audio (track.getAudioPtr()),
      meter (audio->meterL, audio->meterR, false)
{
    nameLabel.setEditable (false, true, false);
    nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    nameLabel.setTooltip (utf8 ("두 번 눌러 이름을 바꿉니다"));
    nameLabel.onTextChange = [this] { session.renameTrack (trackId, nameLabel.getText()); };
    nameLabel.addMouseListener (this, false);
    addAndMakeVisible (nameLabel);

    styleToggle (recButton, DAWColours::record, utf8 ("녹음 준비: 녹음할 때 이 트랙에 녹음하고, 정지 중에는 입력 소리를 들려줍니다"));
    styleToggle (inputButton, DAWColours::input, utf8 ("입력 모니터: 재생 중에도 클립 대신 입력 소리를 들려줍니다"));
    styleToggle (soloButton, DAWColours::solo, utf8 ("솔로: 이 트랙만 듣기"));
    styleToggle (muteButton, DAWColours::mute, utf8 ("음소거"));
    recButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackArmed (trackId, ! t->armed); };
    inputButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackInputMonitor (trackId, ! t->inputMonitor); };
    soloButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackSolo (trackId, ! t->solo); };
    muteButton.onClick = [this] { if (auto* t = session.findTrack (trackId)) session.setTrackMute (trackId, ! t->mute); };
    for (auto* b : { &recButton, &inputButton, &soloButton, &muteButton })
        addAndMakeVisible (b);

    inputBox.setWantsKeyboardFocus (false);
    inputBox.setTooltip (utf8 ("입력: 녹음하고 모니터할 오디오 인터페이스 입력"));
    inputBox.onChange = [this]
    {
        const int id = inputBox.getSelectedId();
        if (id > 0)
            session.setTrackInput (trackId, id > 1000 ? id - 1001 : id - 1, id > 1000);
    };
    addAndMakeVisible (inputBox);

    volume.setSliderStyle (juce::Slider::LinearBar);
    volume.setRange (-60.0, 12.0, 0.1);
    volume.setSkewFactorFromMidPoint (-12.0);
    volume.setDoubleClickReturnValue (true, 0.0);
    volume.setTextValueSuffix (" dB");
    volume.setNumDecimalPlacesToDisplay (1);
    volume.setWantsKeyboardFocus (false);
    volume.setScrollWheelEnabled (false);
    volume.setColour (juce::Slider::trackColourId, DAWColours::accent.withAlpha (0.5f));
    volume.setTooltip (utf8 ("볼륨 (두 번 누르면 0 dB)"));
    volume.onValueChange = [this]
    {
        const double v = volume.getValue();
        session.setTrackVolume (trackId, v <= -60.0 ? TrackAudio::kMinDb : (float) v);
    };
    addAndMakeVisible (volume);
    addAndMakeVisible (meter);

    refresh();
}

void TrackHeader::fillInputMenu (juce::ComboBox& box, int numInputs)
{
    box.clear (juce::dontSendNotification);
    for (int i = 0; i < numInputs; ++i)
        box.addItem (utf8 ("입력 ") + juce::String (i + 1), inputId (i, false));
    for (int i = 0; i + 1 < numInputs; i += 2)
        box.addItem (utf8 ("입력 ") + juce::String (i + 1) + "-" + juce::String (i + 2) + utf8 (" (스테레오)"), inputId (i, true));
}

void TrackHeader::refresh()
{
    auto* track = session.findTrack (trackId);
    if (track == nullptr)
        return;

    if (! nameLabel.isBeingEdited())
        nameLabel.setText (track->name, juce::dontSendNotification);
    recButton.setToggleState (track->armed, juce::dontSendNotification);
    inputButton.setToggleState (track->inputMonitor, juce::dontSendNotification);
    soloButton.setToggleState (track->solo, juce::dontSendNotification);
    muteButton.setToggleState (track->mute, juce::dontSendNotification);

    const int inputs = numDeviceInputs (session);
    if (inputs != shownInputs)
    {
        fillInputMenu (inputBox, std::max (inputs, track->inputChannel + (track->stereoInput ? 2 : 1)));
        shownInputs = inputs;
    }
    inputBox.setSelectedId (inputId (track->inputChannel, track->stereoInput), juce::dontSendNotification);

    if (! volume.isMouseButtonDown())
        volume.setValue (std::max (-60.0f, track->volumeDb), juce::dontSendNotification);
    repaint();
}

void TrackHeader::paint (juce::Graphics& g)
{
    auto* track = session.findTrack (trackId);
    const bool selected = state.selectedTrack == trackId;
    g.fillAll (selected ? DAWColours::panelLight : DAWColours::panel);
    if (track != nullptr)
    {
        g.setColour (track->colour);
        g.fillRect (0, 0, 6, getHeight());
    }
    g.setColour (DAWColours::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

void TrackHeader::resized()
{
    auto r = getLocalBounds().withTrimmedLeft (10).reduced (4, 6);
    auto row1 = r.removeFromTop (24);
    muteButton.setBounds (row1.removeFromRight (26).reduced (1));
    soloButton.setBounds (row1.removeFromRight (26).reduced (1));
    inputButton.setBounds (row1.removeFromRight (26).reduced (1));
    recButton.setBounds (row1.removeFromRight (26).reduced (1));
    nameLabel.setBounds (row1);

    r.removeFromTop (6);
    auto row2 = r.removeFromTop (22);
    inputBox.setBounds (row2.removeFromLeft (92));
    row2.removeFromLeft (6);
    volume.setBounds (row2);

    r.removeFromTop (6);
    meter.setBounds (r.removeFromTop (10));
}

void TrackHeader::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        actions.showTrackMenu (trackId);
        return;
    }

    state.selectedTrack = trackId;
    state.changed();
}

//==============================================================================
EditWindow::EditWindow (Session& s, EditState& e, AppActions& a)
    : session (s), state (e), actions (a), timeline (s, e, a)
{
    headerArea.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (headerArea);
    addAndMakeVisible (timeline);
    for (auto* bar : { &vScroll, &hScroll })
    {
        bar->setAutoHide (false);
        bar->addListener (this);
        addAndMakeVisible (bar);
    }
    timeline.onScrollChanged = [this] { layoutHeaders(); updateScrollBars(); };

    session.addChangeListener (this);
    state.addChangeListener (this);
    syncHeaders();
}

EditWindow::~EditWindow()
{
    state.removeChangeListener (this);
    session.removeChangeListener (this);
}

void EditWindow::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncHeaders();
    for (auto& h : headers)
        h->refresh();
    layoutHeaders();
    updateScrollBars();
}

void EditWindow::syncHeaders()
{
    const auto& tracks = session.getTracks();
    bool same = tracks.size() == headers.size();
    for (size_t i = 0; same && i < tracks.size(); ++i)
        same = headers[i]->getTrackId() == tracks[i]->id;

    if (same)
        return;

    // Keep the headers of tracks that still exist, in the new order.
    std::map<int, std::unique_ptr<TrackHeader>> old;
    for (auto& h : headers)
        old[h->getTrackId()] = std::move (h);
    headers.clear();

    for (const auto& track : tracks)
    {
        auto it = old.find (track->id);
        if (it != old.end())
            headers.push_back (std::move (it->second));
        else
        {
            headers.push_back (std::make_unique<TrackHeader> (session, state, actions, *track));
            headerArea.addAndMakeVisible (*headers.back());
        }
    }

    layoutHeaders();
}

void EditWindow::layoutHeaders()
{
    for (size_t i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (0, (int) i * state.trackHeight - state.scrollY, headerArea.getWidth(), state.trackHeight);
}

void EditWindow::updateScrollBars()
{
    const int visible = std::max (1, timeline.getHeight() - EditState::kRulerHeight);
    const int total = timeline.getLanesHeight() + state.trackHeight / 2;
    vScroll.setRangeLimits (0.0, (double) std::max (total, visible));
    vScroll.setCurrentRange ((double) state.scrollY, (double) visible, juce::dontSendNotification);

    const double visibleSamples = timeline.getWidth() * state.samplesPerPixel;
    const double length = std::max ((double) session.getLength() + visibleSamples * 0.5,
                                    state.scrollSamples + visibleSamples);
    hScroll.setRangeLimits (0.0, std::max (length, visibleSamples));
    hScroll.setCurrentRange (state.scrollSamples, visibleSamples, juce::dontSendNotification);
}

void EditWindow::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &vScroll)
    {
        state.scrollY = (int) start;
        layoutHeaders();
        state.changed();
    }
    else
    {
        state.setScroll (start);
    }
}

void EditWindow::paint (juce::Graphics& g)
{
    g.fillAll (DAWColours::background);

    // The corner above the headers, beside the ruler.
    auto corner = juce::Rectangle<int> (0, 0, EditState::kHeaderWidth, EditState::kRulerHeight);
    g.setColour (DAWColours::panel);
    g.fillRect (corner);
    g.setColour (DAWColours::outline);
    g.drawRect (corner);
    g.drawHorizontalLine (corner.getCentreY(), 0.0f, (float) corner.getRight());
    g.setColour (DAWColours::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (utf8 ("마디|박"), corner.withHeight (corner.getHeight() / 2).reduced (8, 0), juce::Justification::centredRight);
    g.drawText (utf8 ("분:초"), corner.withTrimmedTop (corner.getHeight() / 2).reduced (8, 0), juce::Justification::centredRight);
}

void EditWindow::resized()
{
    auto r = getLocalBounds();
    hScroll.setBounds (r.removeFromBottom (kScrollBarSize).withTrimmedLeft (EditState::kHeaderWidth).withTrimmedRight (kScrollBarSize));
    vScroll.setBounds (r.removeFromRight (kScrollBarSize).withTrimmedTop (EditState::kRulerHeight));
    auto headerColumn = r.removeFromLeft (EditState::kHeaderWidth);
    headerArea.setBounds (headerColumn.withTrimmedTop (EditState::kRulerHeight));
    timeline.setBounds (r);
    layoutHeaders();
    updateScrollBars();
}

void EditWindow::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Wheel over the headers scrolls the tracks like the timeline does.
    timeline.mouseWheelMove (e.getEventRelativeTo (&timeline), wheel);
}
