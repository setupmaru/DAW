#include "TransportBar.h"

#include "CommandIDs.h"
#include "DAWLookAndFeel.h"

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }
}

TransportBar::TransportBar (Session& s, EditState& e, juce::ApplicationCommandManager& c)
    : session (s), state (e), commands (c)
{
    makeTextButton (slipButton, utf8 ("슬립"), CommandIDs::modeSlip, utf8 ("슬립: 클립을 자유롭게 움직입니다 (F2)"));
    makeTextButton (gridButton, utf8 ("그리드"), CommandIDs::modeGrid, utf8 ("그리드: 마디/박자에 맞춰 움직입니다 (F4)"));
    makeTextButton (trimButton, utf8 ("트림"), CommandIDs::toolTrim, utf8 ("트림 도구: 클립 끝을 끌어 길이를 바꿉니다 (F6)"));
    makeTextButton (selectButton, utf8 ("선택"), CommandIDs::toolSelect, utf8 ("선택 도구: 끌어서 구간을 고릅니다 (F7)"));
    makeTextButton (grabButton, utf8 ("잡기"), CommandIDs::toolGrab, utf8 ("잡기 도구: 클립을 고르고 옮깁니다. Alt+끌기는 복사 (F8)"));
    makeTextButton (editButton, utf8 ("편집"), CommandIDs::toggleMixEdit, utf8 ("편집 창 (Ctrl+=)"));
    makeTextButton (mixButton, utf8 ("믹스"), CommandIDs::toggleMixEdit, utf8 ("믹스 창 (Ctrl+=)"));

    for (auto* b : { &slipButton, &gridButton })
        b->setRadioGroupId (1);
    for (auto* b : { &trimButton, &selectButton, &grabButton })
        b->setRadioGroupId (2);
    for (auto* b : { &editButton, &mixButton })
        b->setRadioGroupId (3);

    gridBox.addItemList (EditState::gridNames(), 1);
    gridBox.setWantsKeyboardFocus (false);
    gridBox.setTooltip (utf8 ("그리드 간격"));
    gridBox.onChange = [this]
    {
        state.gridIndex = gridBox.getSelectedItemIndex();
        state.changed();
    };
    addAndMakeVisible (gridBox);

    makeIconButton (toStartButton, CommandIDs::returnToZero, utf8 ("처음으로 (Enter)"));
    makeIconButton (stopButton, CommandIDs::playStop, utf8 ("정지 (Space)"));
    makeIconButton (playButton, CommandIDs::playStop, utf8 ("재생 (Space)"));
    makeIconButton (recordButton, CommandIDs::record, utf8 ("녹음 (Ctrl+Space): [R]로 준비한 트랙에 녹음합니다"));
    makeIconButton (loopButton, CommandIDs::toggleLoop, utf8 ("반복 재생 (Ctrl+L): 선택 구간을 반복합니다"));
    makeIconButton (clickButton, CommandIDs::toggleClick, utf8 ("메트로놈"));
    makeIconButton (zoomOutButton, CommandIDs::zoomOut, utf8 ("축소 (R)"));
    makeIconButton (zoomInButton, CommandIDs::zoomIn, utf8 ("확대 (T)"));

    tempoCaption.setText ("BPM", juce::dontSendNotification);
    tempoCaption.setColour (juce::Label::textColourId, DAWColours::textDim);
    tempoCaption.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (tempoCaption);

    tempoLabel.setEditable (false, true, false);
    tempoLabel.setJustificationType (juce::Justification::centred);
    tempoLabel.setColour (juce::Label::backgroundColourId, DAWColours::background);
    tempoLabel.setColour (juce::Label::outlineColourId, DAWColours::outline);
    tempoLabel.setTooltip (utf8 ("템포: 두 번 눌러 바꿉니다"));
    tempoLabel.onTextChange = [this]
    {
        const double bpm = tempoLabel.getText().getDoubleValue();
        if (bpm >= 20.0 && bpm <= 400.0)
            session.setTempo (bpm);
        refresh();
    };
    addAndMakeVisible (tempoLabel);

    setMixShown (false);
    refresh();
    startTimerHz (20);
}

TransportBar::~TransportBar()
{
    stopTimer();
}

juce::TextButton& TransportBar::makeTextButton (juce::TextButton& b, const juce::String& text, int commandId, const juce::String& tip)
{
    b.setButtonText (text);
    b.setTooltip (tip);
    b.setWantsKeyboardFocus (false);
    b.setClickingTogglesState (false);
    b.onClick = [this, commandId] { commands.invokeDirectly (commandId, true); };
    addAndMakeVisible (b);
    return b;
}

void TransportBar::makeIconButton (IconButton& b, int commandId, const juce::String& tip)
{
    b.setTooltip (tip);
    b.onClick = [this, commandId, &b]
    {
        // Stop does nothing while stopped, play nothing while playing.
        if (&b == &stopButton && ! session.getEngine().isPlaying())
            return;
        if (&b == &playButton && session.getEngine().isPlaying())
            return;
        commands.invokeDirectly (commandId, true);
    };
    addAndMakeVisible (b);
}

void TransportBar::setMixShown (bool mix)
{
    editButton.setToggleState (! mix, juce::dontSendNotification);
    mixButton.setToggleState (mix, juce::dontSendNotification);
}

void TransportBar::timerCallback()
{
    refresh();
}

void TransportBar::refresh()
{
    auto& engine = session.getEngine();
    state.tempo = session.getTempo();

    slipButton.setToggleState (state.mode == EditMode::slip, juce::dontSendNotification);
    gridButton.setToggleState (state.mode == EditMode::grid, juce::dontSendNotification);
    trimButton.setToggleState (state.tool == EditTool::trim, juce::dontSendNotification);
    selectButton.setToggleState (state.tool == EditTool::select, juce::dontSendNotification);
    grabButton.setToggleState (state.tool == EditTool::grab, juce::dontSendNotification);
    if (gridBox.getSelectedItemIndex() != state.gridIndex)
        gridBox.setSelectedItemIndex (state.gridIndex, juce::dontSendNotification);
    gridBox.setEnabled (state.mode == EditMode::grid);

    const bool playing = engine.isPlaying();
    const bool recording = engine.isRecording();
    playButton.setToggleState (playing, juce::dontSendNotification);
    stopButton.setToggleState (false, juce::dontSendNotification);
    if (++blinkTicks >= 8)
    {
        blinkTicks = 0;
        recordBlink = ! recordBlink;
    }
    recordButton.setToggleState (recording && (recordBlink || playing), juce::dontSendNotification);
    loopButton.setToggleState (engine.isLoopEnabled(), juce::dontSendNotification);
    clickButton.setToggleState (engine.isClickEnabled(), juce::dontSendNotification);

    if (! tempoLabel.isBeingEdited())
        tempoLabel.setText (juce::String (session.getTempo(), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."),
                            juce::dontSendNotification);

    const auto position = playing ? engine.getPosition() : state.cursor;
    auto main = state.formatMain (position);
    auto sub = state.showBars ? state.formatMinSec (position) : state.formatBars (position);
    juce::String sel;
    if (! state.selection.isEmpty())
        sel = utf8 ("시작 ") + state.formatMain (state.selection.start) + "\n"
              + utf8 ("끝    ") + state.formatMain (state.selection.end) + "\n"
              + utf8 ("길이 ") + state.formatMinSec (state.selection.length());

    if (main != mainCounter || sub != subCounter || sel != selectionText)
    {
        mainCounter = main;
        subCounter = sub;
        selectionText = sel;
        repaint (counterArea.getUnion (selectionArea));
    }
}

void TransportBar::mouseDown (const juce::MouseEvent& e)
{
    if (counterArea.contains (e.getPosition()))
        commands.invokeDirectly (CommandIDs::toggleCounter, true);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (DAWColours::panel);
    g.setColour (DAWColours::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

    // Counter: a dark display.
    auto c = counterArea.toFloat();
    g.setColour (juce::Colour (0xff101113));
    g.fillRoundedRectangle (c, 5.0f);
    g.setColour (DAWColours::outline);
    g.drawRoundedRectangle (c, 5.0f, 1.0f);

    auto inner = counterArea.reduced (10, 4);
    g.setColour (juce::Colour (0xff7fc4ff));
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText (mainCounter, inner.removeFromTop (inner.getHeight() * 2 / 3), juce::Justification::centredLeft);
    g.setColour (DAWColours::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (subCounter + (state.showBars ? utf8 ("   (분:초)") : utf8 ("   (마디|박)")), inner, juce::Justification::centredLeft);

    g.setColour (DAWColours::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (selectionText, selectionArea, juce::Justification::centredLeft, 3);
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced (8, 8);
    const int h = r.getHeight();

    auto group = [&] (int width) { auto a = r.removeFromLeft (width); r.removeFromLeft (10); return a; };

    auto modes = group (150);
    auto modeTop = modes.removeFromTop (h / 2);
    slipButton.setBounds (modeTop.removeFromLeft (modeTop.getWidth() / 2).reduced (1));
    gridButton.setBounds (modeTop.reduced (1));
    gridBox.setBounds (modes.reduced (1));

    auto tools = group (150);
    trimButton.setBounds (tools.removeFromLeft (50).reduced (1));
    selectButton.setBounds (tools.removeFromLeft (50).reduced (1));
    grabButton.setBounds (tools.reduced (1));

    auto transport = group (6 * 44);
    for (auto* b : { (juce::Component*) &toStartButton, (juce::Component*) &stopButton, (juce::Component*) &playButton,
                     (juce::Component*) &recordButton, (juce::Component*) &loopButton, (juce::Component*) &clickButton })
        b->setBounds (transport.removeFromLeft (44).reduced (2));

    counterArea = group (220);
    selectionArea = group (170);

    auto right = r.removeFromRight (130);
    auto rightTop = right.removeFromTop (h / 2);
    editButton.setBounds (rightTop.removeFromLeft (65).reduced (1));
    mixButton.setBounds (rightTop.reduced (1));
    zoomOutButton.setBounds (right.removeFromLeft (65).reduced (1));
    zoomInButton.setBounds (right.reduced (1));

    auto tempo = r.removeFromRight (110);
    tempoCaption.setBounds (tempo.removeFromLeft (38));
    tempoLabel.setBounds (tempo.withSizeKeepingCentre (tempo.getWidth() - 6, 26));
}
