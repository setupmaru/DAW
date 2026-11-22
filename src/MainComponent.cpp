#include "MainComponent.h"

#include "session/SessionFile.h"
#include "ui/CommandIDs.h"
#include "ui/DAWLookAndFeel.h"

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

constexpr const char* kDeviceSettingKey = "audioDevice";
constexpr const char* kLastFolderKey = "lastFolder";
constexpr int kStatusHeight = 24;
constexpr int kTransportHeight = 64;
const juce::String kAudioWildcard = "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3";

std::unique_ptr<juce::PropertiesFile> openSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "settings";
    options.filenameSuffix = ".xml";
    options.folderName = "DAW";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return std::make_unique<juce::PropertiesFile> (options);
}
} // namespace

//==============================================================================
// Covers the window while a bounce runs: progress and a cancel button.
class MainComponent::BusyOverlay : public juce::Component, private juce::Timer
{
public:
    BusyOverlay (std::function<float()> p, std::function<void()> cancel)
        : progress (std::move (p))
    {
        cancelButton.setButtonText (utf8 ("취소"));
        cancelButton.onClick = std::move (cancel);
        addAndMakeVisible (cancelButton);
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        auto box = getLocalBounds().withSizeKeepingCentre (380, 130);
        g.setColour (DAWColours::panel);
        g.fillRoundedRectangle (box.toFloat(), 8.0f);
        g.setColour (DAWColours::text);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText (utf8 ("믹스다운 내보내는 중..."), box.removeFromTop (44), juce::Justification::centred);
        auto bar = box.removeFromTop (18).reduced (24, 0).toFloat();
        g.setColour (DAWColours::background);
        g.fillRoundedRectangle (bar, 4.0f);
        g.setColour (DAWColours::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress())), 4.0f);
    }

    void resized() override
    {
        cancelButton.setBounds (getLocalBounds().withSizeKeepingCentre (380, 130).removeFromBottom (48).withSizeKeepingCentre (100, 28));
    }

    void mouseDown (const juce::MouseEvent&) override {}   // blocks clicks to the window

private:
    void timerCallback() override { repaint(); }

    std::function<float()> progress;
    juce::TextButton cancelButton;
};

//==============================================================================
MainComponent::MainComponent()
    : settings (openSettings()),
      scanner (pluginFormats, PluginScanner::defaultCacheFile())
{
    pluginFormats.addFormat (std::make_unique<juce::VST3PluginFormat>());
    audioFormats.registerBasicFormats();
    scanner.loadCache();

    const auto savedDevice = settings->getXmlValue (kDeviceSettingKey);
    startupError = engine.initialise (savedDevice.get());
    engine.getDeviceManager().addChangeListener (this);

    session = std::make_unique<Session> (engine, pluginFormats, audioFormats);
    session->onError = [this] (const juce::String& text) { showMessage (text); };
    session->onTrackRemoving = [this] (int trackId) { closeEditorsOf (trackId); };
    session->onInsertsChanged = [this] { closeStaleEditors(); };
    session->addChangeListener (this);
    session->reset (engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 48000.0);
    session->addTrack();
    session->setDirty (false);

    editState.sampleRate = session->getSampleRate();
    editState.selectedTrack = session->getTracks().empty() ? 0 : session->getTracks().front()->id;
    editState.addChangeListener (this);

    commandManager.registerAllCommandsForTarget (this);
    addKeyListener (commandManager.getKeyMappings());
    setWantsKeyboardFocus (true);

    menuBar = std::make_unique<juce::MenuBarComponent> (this);
    addAndMakeVisible (*menuBar);
    transport = std::make_unique<TransportBar> (*session, editState, commandManager);
    addAndMakeVisible (*transport);
    editWindow = std::make_unique<EditWindow> (*session, editState, *this);
    addAndMakeVisible (*editWindow);
    mixWindow = std::make_unique<MixWindow> (*session, *this);
    addChildComponent (*mixWindow);

    if (startupError.isNotEmpty())
        showMessage (startupError);
    else if (scanner.getKnownPlugins().getNumTypes() == 0)
        showMessage (utf8 ("VST3 플러그인 목록이 비어 있습니다. [설정] → [플러그인 다시 스캔]으로 찾을 수 있습니다."));

    setSize (1280, 800);
    startTimerHz (4);
    deviceChanged();
}

MainComponent::~MainComponent()
{
    stopTimer();
    bouncer.reset();
    overlay.reset();
    editorWindows.clear();
    if (settingsDialog != nullptr)
        delete settingsDialog.getComponent();

    if (const auto state = engine.getDeviceManager().createStateXml())
        settings->setValue (kDeviceSettingKey, state.get());
    settings->saveIfNeeded();

    engine.getDeviceManager().removeChangeListener (this);
    editState.removeChangeListener (this);
    session->removeChangeListener (this);

    // Windows first, then the session, while the engine is still alive.
    mixWindow.reset();
    editWindow.reset();
    transport.reset();
    menuBar.reset();
    session.reset();
    engine.shutdown();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (DAWColours::background);

    auto status = statusArea;
    g.setColour (DAWColours::panel);
    g.fillRect (status);
    g.setColour (DAWColours::outline);
    g.drawHorizontalLine (status.getY(), 0.0f, (float) getWidth());

    status = status.reduced (10, 0);
    juce::String device = utf8 ("오디오 장치 없음");
    if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
    {
        const double rate = d->getCurrentSampleRate();
        const int buffer = d->getCurrentBufferSizeSamples();
        const double latencyMs = (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples()) * 1000.0 / std::max (1.0, rate);
        device = d->getName() + "  ·  " + juce::String ((int) rate) + " Hz  ·  " + utf8 ("버퍼 ") + juce::String (buffer)
                 + "  ·  " + utf8 ("지연 ") + juce::String (latencyMs, 1) + " ms  ·  CPU "
                 + juce::String ((int) std::round (engine.getDeviceManager().getCpuUsage() * 100.0)) + "%";
    }

    g.setFont (juce::FontOptions (12.5f));
    g.setColour (DAWColours::textDim);
    g.drawText (device, status, juce::Justification::centredRight);

    auto left = session->getName() + (session->isDirty() ? " *" : "") + "  ·  "
                + juce::String ((int) session->getSampleRate()) + " Hz  ·  "
                + juce::String ((int) session->getTracks().size()) + utf8 ("개 트랙");
    g.setColour (DAWColours::text);
    g.drawText (left, status.removeFromLeft (360), juce::Justification::centredLeft, true);

    const auto& note = juce::Time::getMillisecondCounter() < messageUntil ? message : persistentWarning;
    if (note.isNotEmpty())
    {
        g.setColour (note == persistentWarning ? DAWColours::mute : DAWColours::playhead);
        g.drawText (note, status.withTrimmedRight (420), juce::Justification::centredLeft, true);
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    menuBar->setBounds (r.removeFromTop (juce::LookAndFeel::getDefaultLookAndFeel().getDefaultMenuBarHeight()));
    transport->setBounds (r.removeFromTop (kTransportHeight));
    statusArea = r.removeFromBottom (kStatusHeight);
    editWindow->setBounds (r);
    mixWindow->setBounds (r);
    if (overlay != nullptr)
        overlay->setBounds (getLocalBounds());
}

void MainComponent::timerCallback()
{
    repaint (statusArea);
    updateTitle();
}

void MainComponent::updateTitle()
{
    const auto title = "DAW - " + session->getName() + (session->isDirty() ? " *" : "");
    if (title != lastWindowTitle)
    {
        lastWindowTitle = title;
        if (auto* window = getTopLevelComponent(); window != nullptr && window != this)
            window->setName (title);
    }
}

void MainComponent::showMessage (const juce::String& text)
{
    message = text;
    messageUntil = juce::Time::getMillisecondCounter() + 6000;
    repaint (statusArea);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.getDeviceManager())
    {
        deviceChanged();
        return;
    }

    if (source == &editState)
        syncLoop();

    editState.sampleRate = session->getSampleRate();
    editState.tempo = session->getTempo();
    repaint (statusArea);
}

void MainComponent::deviceChanged()
{
    if (const auto state = engine.getDeviceManager().createStateXml())
        settings->setValue (kDeviceSettingKey, state.get());

    persistentWarning.clear();
    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
    {
        startupError.clear();
        const double rate = device->getCurrentSampleRate();
        if (! session->adoptSampleRate (rate))
            persistentWarning = utf8 ("장치 샘플레이트(") + juce::String ((int) rate) + utf8 (" Hz)가 세션(")
                                + juce::String ((int) session->getSampleRate()) + utf8 (" Hz)과 달라 빠르거나 느리게 들립니다. [오디오 설정]에서 맞추세요.");
        editState.sampleRate = session->getSampleRate();
    }
    else
    {
        persistentWarning = startupError.isNotEmpty() ? startupError : utf8 ("오디오 장치가 없습니다. [설정] → [오디오 설정]에서 고르세요.");
    }

    session->sendChangeMessage();   // headers refresh their input lists
    repaint();
}

//==============================================================================
std::vector<int> MainComponent::editTracks() const
{
    std::vector<int> ids;
    if (! editState.selection.trackIds.empty())
    {
        for (const auto& t : session->getTracks())
            if (editState.selection.trackIds.count (t->id) != 0)
                ids.push_back (t->id);
        return ids;
    }

    for (const int clipId : editState.selectedClips)
    {
        int trackId = 0;
        if (session->findClip (clipId, &trackId) != nullptr)
            ids.push_back (trackId);
    }
    if (! ids.empty())
        return ids;

    if (session->findTrack (editState.selectedTrack) != nullptr)
        return { editState.selectedTrack };

    for (const auto& t : session->getTracks())
        ids.push_back (t->id);
    return ids;
}

void MainComponent::getAllCommands (juce::Array<juce::CommandID>& c)
{
    for (const juce::CommandID id : std::initializer_list<juce::CommandID> { CommandIDs::newSession, CommandIDs::openSession, CommandIDs::saveSession, CommandIDs::saveSessionAs, CommandIDs::importAudio, CommandIDs::bounce, CommandIDs::audioSettings, CommandIDs::rescanPlugins,
                  juce::StandardApplicationCommandIDs::quit,
                  CommandIDs::undo, CommandIDs::redo, CommandIDs::deleteSelection, CommandIDs::split, CommandIDs::duplicate, CommandIDs::selectAll,
                  CommandIDs::newTrack, CommandIDs::deleteTrack,
                  CommandIDs::playStop, CommandIDs::record, CommandIDs::returnToZero, CommandIDs::toggleLoop, CommandIDs::toggleClick,
                  CommandIDs::toolTrim, CommandIDs::toolSelect, CommandIDs::toolGrab, CommandIDs::modeSlip, CommandIDs::modeGrid, CommandIDs::zoomIn, CommandIDs::zoomOut, CommandIDs::zoomFit, CommandIDs::toggleMixEdit, CommandIDs::toggleCounter })
        c.add (id);
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto ctrl = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    auto& undoManager = session->getUndoManager();

    switch (id)
    {
        case CommandIDs::newSession:     info.setInfo (utf8 ("새 세션"), {}, "File", 0); info.addDefaultKeypress ('n', ctrl); break;
        case CommandIDs::openSession:    info.setInfo (utf8 ("세션 열기..."), {}, "File", 0); info.addDefaultKeypress ('o', ctrl); break;
        case CommandIDs::saveSession:    info.setInfo (utf8 ("저장"), {}, "File", 0); info.addDefaultKeypress ('s', ctrl); break;
        case CommandIDs::saveSessionAs:  info.setInfo (utf8 ("다른 이름으로 저장..."), {}, "File", 0); info.addDefaultKeypress ('s', ctrl | shift); break;
        case CommandIDs::importAudio:    info.setInfo (utf8 ("오디오 가져오기..."), {}, "File", 0); info.addDefaultKeypress ('i', ctrl | shift); break;
        case CommandIDs::bounce:         info.setInfo (utf8 ("믹스다운 내보내기 (WAV)..."), {}, "File", 0); info.addDefaultKeypress ('b', ctrl | alt); break;
        case CommandIDs::audioSettings:  info.setInfo (utf8 ("오디오 설정..."), {}, "Settings", 0); break;
        case CommandIDs::rescanPlugins:  info.setInfo (scanner.isScanning() ? utf8 ("플러그인 스캔 중...") : utf8 ("플러그인 다시 스캔"), {}, "Settings", 0);
                             info.setActive (! scanner.isScanning()); break;
        case juce::StandardApplicationCommandIDs::quit:
                             info.setInfo (utf8 ("끝내기"), {}, "File", 0); info.addDefaultKeypress ('q', ctrl); break;

        case CommandIDs::undo:           info.setInfo (utf8 ("실행 취소 ") + undoManager.getUndoDescription(), {}, "Edit", 0);
                             info.setActive (undoManager.canUndo()); info.addDefaultKeypress ('z', ctrl); break;
        case CommandIDs::redo:           info.setInfo (utf8 ("다시 실행 ") + undoManager.getRedoDescription(), {}, "Edit", 0);
                             info.setActive (undoManager.canRedo()); info.addDefaultKeypress ('z', ctrl | shift);
                             info.addDefaultKeypress ('y', ctrl); break;
        case CommandIDs::deleteSelection:info.setInfo (utf8 ("삭제"), {}, "Edit", 0);
                             info.addDefaultKeypress (juce::KeyPress::deleteKey, 0);
                             info.addDefaultKeypress (juce::KeyPress::backspaceKey, 0); break;
        case CommandIDs::split:          info.setInfo (utf8 ("클립 분할"), {}, "Edit", 0); info.addDefaultKeypress ('e', ctrl);
                             info.addDefaultKeypress ('b', 0); break;
        case CommandIDs::duplicate:      info.setInfo (utf8 ("복제"), {}, "Edit", 0); info.addDefaultKeypress ('d', ctrl); break;
        case CommandIDs::selectAll:      info.setInfo (utf8 ("모든 클립 선택"), {}, "Edit", 0); info.addDefaultKeypress ('a', ctrl); break;

        case CommandIDs::newTrack:       info.setInfo (utf8 ("새 트랙"), {}, "Track", 0); info.addDefaultKeypress ('n', ctrl | shift); break;
        case CommandIDs::deleteTrack:    info.setInfo (utf8 ("선택한 트랙 삭제"), {}, "Track", 0);
                             info.setActive (session->findTrack (editState.selectedTrack) != nullptr); break;

        case CommandIDs::playStop:       info.setInfo (utf8 ("재생 / 정지"), {}, "Transport", 0); info.addDefaultKeypress (juce::KeyPress::spaceKey, 0); break;
        case CommandIDs::record:         info.setInfo (utf8 ("녹음"), {}, "Transport", 0); info.addDefaultKeypress (juce::KeyPress::spaceKey, ctrl);
                             info.addDefaultKeypress (juce::KeyPress::F12Key, 0); break;
        case CommandIDs::returnToZero:   info.setInfo (utf8 ("처음으로"), {}, "Transport", 0); info.addDefaultKeypress (juce::KeyPress::returnKey, 0);
                             info.addDefaultKeypress (juce::KeyPress::homeKey, 0); break;
        case CommandIDs::toggleLoop:     info.setInfo (utf8 ("반복 재생"), {}, "Transport", 0); info.setTicked (engine.isLoopEnabled());
                             info.addDefaultKeypress ('l', ctrl); break;
        case CommandIDs::toggleClick:    info.setInfo (utf8 ("메트로놈"), {}, "Transport", 0); info.setTicked (engine.isClickEnabled()); break;

        case CommandIDs::toolTrim:       info.setInfo (utf8 ("트림 도구"), {}, "Tools", 0); info.setTicked (editState.tool == EditTool::trim);
                             info.addDefaultKeypress (juce::KeyPress::F6Key, 0); break;
        case CommandIDs::toolSelect:     info.setInfo (utf8 ("선택 도구"), {}, "Tools", 0); info.setTicked (editState.tool == EditTool::select);
                             info.addDefaultKeypress (juce::KeyPress::F7Key, 0); break;
        case CommandIDs::toolGrab:       info.setInfo (utf8 ("잡기 도구"), {}, "Tools", 0); info.setTicked (editState.tool == EditTool::grab);
                             info.addDefaultKeypress (juce::KeyPress::F8Key, 0); break;
        case CommandIDs::modeSlip:       info.setInfo (utf8 ("슬립 모드"), {}, "Tools", 0); info.setTicked (editState.mode == EditMode::slip);
                             info.addDefaultKeypress (juce::KeyPress::F2Key, 0); break;
        case CommandIDs::modeGrid:       info.setInfo (utf8 ("그리드 모드"), {}, "Tools", 0); info.setTicked (editState.mode == EditMode::grid);
                             info.addDefaultKeypress (juce::KeyPress::F4Key, 0); break;
        case CommandIDs::zoomIn:         info.setInfo (utf8 ("확대"), {}, "View", 0); info.addDefaultKeypress ('t', 0); break;
        case CommandIDs::zoomOut:        info.setInfo (utf8 ("축소"), {}, "View", 0); info.addDefaultKeypress ('r', 0); break;
        case CommandIDs::zoomFit:        info.setInfo (utf8 ("세션 전체 보기"), {}, "View", 0); info.addDefaultKeypress ('f', ctrl | alt); break;
        case CommandIDs::toggleMixEdit:  info.setInfo (mixShown ? utf8 ("편집 창 보기") : utf8 ("믹스 창 보기"), {}, "View", 0);
                             info.addDefaultKeypress ('=', ctrl); break;
        case CommandIDs::toggleCounter:  info.setInfo (utf8 ("카운터: 마디|박 / 분:초"), {}, "View", 0); break;
        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{

    if (bouncer != nullptr)
        return true;   // nothing while bouncing

    // Clip edits wait while the mouse drags clips: the drag rebuilds the
    // clips from its start state and would overwrite them.
    switch (info.commandID)
    {
        case CommandIDs::undo: case CommandIDs::redo: case CommandIDs::deleteSelection:
        case CommandIDs::split: case CommandIDs::duplicate:
        case CommandIDs::newSession: case CommandIDs::openSession: case CommandIDs::importAudio:
            if (editWindow->getTimeline().isDragging())
                return true;
            break;
        default: break;
    }

    switch (info.commandID)
    {
        case CommandIDs::newSession:    confirmDiscard ([this] { newSession(); }); break;
        case CommandIDs::openSession:   confirmDiscard ([this] { openSessionChooser(); }); break;
        case CommandIDs::saveSession:   saveSession(); break;
        case CommandIDs::saveSessionAs: saveSessionAs(); break;
        case CommandIDs::importAudio:   importChooser(); break;
        case CommandIDs::bounce:        bounceChooser(); break;
        case CommandIDs::audioSettings: showAudioSettings(); break;
        case CommandIDs::rescanPlugins:
            showMessage (utf8 ("VST3 플러그인을 찾는 중입니다..."));
            scanner.startScan ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->showMessage (utf8 ("플러그인 스캔 완료: ") + juce::String (safe->scanner.getKnownPlugins().getNumTypes()) + utf8 ("개"));
            });
            break;
        case juce::StandardApplicationCommandIDs::quit:
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            break;

        case CommandIDs::undo: session->getUndoManager().undo(); editState.selectedClips.clear(); editState.changed(); break;
        case CommandIDs::redo: session->getUndoManager().redo(); editState.selectedClips.clear(); editState.changed(); break;

        case CommandIDs::deleteSelection:
            if (! editState.selection.isEmpty())
            {
                std::vector<int> ids (editState.selection.trackIds.begin(), editState.selection.trackIds.end());
                session->deleteRange (ids, editState.selection.start, editState.selection.end);
                editState.selection = {};
            }
            else if (! editState.selectedClips.empty())
            {
                session->deleteClips (editState.selectedClips);
                editState.selectedClips.clear();
            }
            editState.changed();
            break;

        case CommandIDs::split:
        {
            const auto tracks = editTracks();
            if (! editState.selection.isEmpty())
            {
                session->editClips (utf8 ("분할"), [&] (Session::ClipMap& map)
                {
                    for (const int id : tracks)
                    {
                        ClipEdit::splitAt (map[id], editState.selection.start, session->idSource());
                        ClipEdit::splitAt (map[id], editState.selection.end, session->idSource());
                    }
                });
            }
            else
            {
                session->splitClips (tracks, editState.cursor);
            }
            editState.selectedClips.clear();
            editState.changed();
            break;
        }

        case CommandIDs::duplicate:
            if (! editState.selectedClips.empty())
            {
                editState.selectedClips = session->duplicateClips (editState.selectedClips);
                editState.changed();
            }
            break;

        case CommandIDs::selectAll:
            editState.selectedClips.clear();
            for (const auto& t : session->getTracks())
                for (const auto& c : t->clips)
                    editState.selectedClips.insert (c.id);
            editState.selection = {};
            editState.changed();
            break;

        case CommandIDs::newTrack:
            if (auto* t = session->addTrack())
            {
                editState.selectedTrack = t->id;
                editState.changed();
            }
            break;
        case CommandIDs::deleteTrack: deleteTrack (editState.selectedTrack); break;

        case CommandIDs::playStop:     togglePlay(); break;
        case CommandIDs::record:       toggleRecord(); break;
        case CommandIDs::returnToZero:
            editState.cursor = 0;
            editState.selection = {};
            locate (0);
            editState.setScroll (0.0);
            break;
        case CommandIDs::toggleLoop:
            if (engine.isLoopEnabled())
                engine.setLoop (false, 0, 0);
            else if (editState.selection.isEmpty())
                showMessage (utf8 ("반복할 구간을 먼저 선택 도구로 고르세요."));
            else
                engine.setLoop (true, editState.selection.start, editState.selection.end);
            editWindow->repaint();
            break;
        case CommandIDs::toggleClick:  engine.setClickEnabled (! engine.isClickEnabled()); break;

        case CommandIDs::toolTrim:   editState.tool = EditTool::trim; editState.changed(); break;
        case CommandIDs::toolSelect: editState.tool = EditTool::select; editState.changed(); break;
        case CommandIDs::toolGrab:   editState.tool = EditTool::grab; editState.changed(); break;
        case CommandIDs::modeSlip:   editState.mode = EditMode::slip; editState.changed(); break;
        case CommandIDs::modeGrid:   editState.mode = EditMode::grid; editState.changed(); break;
        case CommandIDs::zoomIn:     zoomBy (0.5); break;
        case CommandIDs::zoomOut:    zoomBy (2.0); break;
        case CommandIDs::zoomFit:    zoomToFit(); break;
        case CommandIDs::toggleMixEdit: showMix (! mixShown); break;
        case CommandIDs::toggleCounter: editState.showBars = ! editState.showBars; editState.changed(); break;
        default: return false;
    }

    commandManager.commandStatusChanged();
    return true;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { utf8 ("파일"), utf8 ("편집"), utf8 ("트랙"), utf8 ("재생"), utf8 ("보기"), utf8 ("설정") };
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu m;
    auto* cm = &commandManager;

    switch (index)
    {
        case 0:
            m.addCommandItem (cm, CommandIDs::newSession);
            m.addCommandItem (cm, CommandIDs::openSession);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::saveSession);
            m.addCommandItem (cm, CommandIDs::saveSessionAs);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::importAudio);
            m.addCommandItem (cm, CommandIDs::bounce);
            m.addSeparator();
            m.addCommandItem (cm, juce::StandardApplicationCommandIDs::quit);
            break;
        case 1:
            m.addCommandItem (cm, CommandIDs::undo);
            m.addCommandItem (cm, CommandIDs::redo);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::split);
            m.addCommandItem (cm, CommandIDs::duplicate);
            m.addCommandItem (cm, CommandIDs::deleteSelection);
            m.addCommandItem (cm, CommandIDs::selectAll);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::toolTrim);
            m.addCommandItem (cm, CommandIDs::toolSelect);
            m.addCommandItem (cm, CommandIDs::toolGrab);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::modeSlip);
            m.addCommandItem (cm, CommandIDs::modeGrid);
            break;
        case 2:
            m.addCommandItem (cm, CommandIDs::newTrack);
            m.addCommandItem (cm, CommandIDs::deleteTrack);
            break;
        case 3:
            m.addCommandItem (cm, CommandIDs::playStop);
            m.addCommandItem (cm, CommandIDs::record);
            m.addCommandItem (cm, CommandIDs::returnToZero);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::toggleLoop);
            m.addCommandItem (cm, CommandIDs::toggleClick);
            break;
        case 4:
            m.addCommandItem (cm, CommandIDs::toggleMixEdit);
            m.addSeparator();
            m.addCommandItem (cm, CommandIDs::zoomIn);
            m.addCommandItem (cm, CommandIDs::zoomOut);
            m.addCommandItem (cm, CommandIDs::zoomFit);
            m.addCommandItem (cm, CommandIDs::toggleCounter);
            break;
        case 5:
            m.addCommandItem (cm, CommandIDs::audioSettings);
            m.addCommandItem (cm, CommandIDs::rescanPlugins);
            break;
        default: break;
    }

    return m;
}

//==============================================================================
void MainComponent::togglePlay()
{
    if (engine.isPlaying() || engine.isRecording())
    {
        engine.stop();
        engine.locate (editState.cursor);   // back to where playback started
        return;
    }

    const auto from = editState.selection.isEmpty() ? editState.cursor : editState.selection.start;
    engine.locate (from);
    syncLoop();
    if (! engine.play())
        showMessage (utf8 ("오디오 장치가 열려 있지 않아 재생할 수 없습니다. [설정] → [오디오 설정]에서 장치를 고르세요."));
}

void MainComponent::toggleRecord()
{
    if (engine.isRecording())
    {
        engine.stop();
        engine.locate (editState.cursor);
        return;
    }

    if (engine.isPlaying())
        engine.stop();

    engine.setLoop (false, 0, 0);
    const auto from = editState.selection.isEmpty() ? editState.cursor : editState.selection.start;
    engine.locate (from);
    editState.cursor = from;
    if (const auto error = session->startRecording(); error.isNotEmpty())
        showMessage (error);
}

void MainComponent::syncLoop()
{
    if (! engine.isLoopEnabled())
        return;

    if (editState.selection.isEmpty())
        engine.setLoop (false, 0, 0);
    else
        engine.setLoop (true, editState.selection.start, editState.selection.end);
}

void MainComponent::locate (juce::int64 position)
{
    engine.locate (position);
}

void MainComponent::zoomBy (double factor)
{
    const auto anchorTime = engine.isPlaying() ? engine.getPosition() : editState.cursor;
    float x = editState.timeToX (anchorTime);
    if (x < 0.0f || x > (float) editWindow->getTimelineWidth())
        x = (float) editWindow->getTimelineWidth() * 0.5f;
    editState.zoom (factor, x);
}

void MainComponent::zoomToFit()
{
    const double length = std::max ((double) session->getLength(), session->getSampleRate() * 10.0);
    editState.samplesPerPixel = juce::jlimit (EditState::kMinSamplesPerPixel, EditState::kMaxSamplesPerPixel,
                                              length * 1.05 / std::max (100, editWindow->getTimelineWidth()));
    editState.setScroll (0.0);
}

void MainComponent::showMix (bool mix)
{
    mixShown = mix;
    mixWindow->setVisible (mix);
    editWindow->setVisible (! mix);
    transport->setMixShown (mix);
    commandManager.commandStatusChanged();
}

//==============================================================================
PluginChain* MainComponent::chainFor (int trackId)
{
    if (trackId == kMasterId)
        return &session->getMasterInserts();
    auto* track = session->findTrack (trackId);
    return track != nullptr ? &track->getInserts() : nullptr;
}

void MainComponent::closeStaleEditors()
{
    for (auto it = editorWindows.begin(); it != editorWindows.end();)
    {
        bool alive = false;
        if (auto* chain = chainFor (it->first.first))
            for (const auto& entry : chain->getEntries())
                alive = alive || entry.id == it->first.second;

        it = alive ? std::next (it) : editorWindows.erase (it);
    }
}

void MainComponent::closeEditorsOf (int trackId)
{
    for (auto it = editorWindows.begin(); it != editorWindows.end();)
        it = it->first.first == trackId ? editorWindows.erase (it) : std::next (it);
}

void MainComponent::openPluginEditor (int trackId, int entryId)
{
    const auto key = std::make_pair (trackId, entryId);
    if (auto it = editorWindows.find (key); it != editorWindows.end())
    {
        it->second->toFront (true);
        return;
    }

    auto* chain = chainFor (trackId);
    if (chain == nullptr)
        return;

    const ChainEntry* entry = nullptr;
    for (const auto& e : chain->getEntries())
        if (e.id == entryId)
            entry = &e;
    if (entry == nullptr || entry->processor == nullptr)
        return;

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    if (entry->processor->hasEditor())
        editor.reset (entry->processor->createEditorAndMakeActive());
    if (editor == nullptr)
        editor = std::make_unique<juce::GenericAudioProcessorEditor> (*entry->processor);

    auto* track = session->findTrack (trackId);
    const auto title = (track != nullptr ? track->name : utf8 ("마스터")) + " - " + entry->name;
    editorWindows[key] = std::make_unique<PluginEditorWindow> (title, std::move (editor),
                                                               [this, key] { editorWindows.erase (key); });
}

void MainComponent::showInsertMenu (int trackId, int entryId, juce::Component& anchor)
{
    auto* chain = chainFor (trackId);
    if (chain == nullptr)
        return;

    juce::PopupMenu menu;
    auto safe = juce::Component::SafePointer<MainComponent> (this);

    if (entryId < 0)
    {
        auto types = scanner.getKnownPlugins().getTypes();
        types.removeIf ([] (const juce::PluginDescription& d) { return d.isInstrument; });

        if (types.isEmpty())
        {
            menu.addItem (1, utf8 ("플러그인 목록이 비어 있습니다 - 지금 스캔"));
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [safe] (int result)
            {
                if (safe != nullptr && result == 1)
                    safe->commandManager.invokeDirectly (CommandIDs::rescanPlugins, false);
            });
            return;
        }

        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [safe, trackId, types] (int result)
        {
            if (safe == nullptr)
                return;
            const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);
            if (auto* c = safe->chainFor (trackId); c != nullptr && index >= 0)
            {
                safe->showMessage (utf8 ("플러그인을 불러오는 중: ") + types[index].name);
                c->addPlugin (types[index]);
            }
        });
        return;
    }

    const auto& entries = chain->getEntries();
    int index = -1;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == entryId)
            index = (int) i;
    if (index < 0)
        return;

    const bool bypassed = entries[(size_t) index].bypassed;
    menu.addSectionHeader (entries[(size_t) index].name);
    menu.addItem (1, utf8 ("플러그인 창 열기"));
    menu.addItem (2, utf8 ("바이패스"), true, bypassed);
    menu.addSeparator();
    menu.addItem (3, utf8 ("위로"), index > 0);
    menu.addItem (4, utf8 ("아래로"), index + 1 < (int) entries.size());
    menu.addSeparator();
    menu.addItem (5, utf8 ("제거"));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor),
                        [safe, trackId, entryId, index, bypassed] (int result)
    {
        if (safe == nullptr)
            return;
        auto* c = safe->chainFor (trackId);
        if (c == nullptr)
            return;

        switch (result)
        {
            case 1: safe->openPluginEditor (trackId, entryId); break;
            case 2: c->setBypassed (entryId, ! bypassed); break;
            case 3: c->move (entryId, index - 1); break;
            case 4: c->move (entryId, index + 1); break;
            case 5: c->remove (entryId); break;
            default: break;
        }
    });
}

//==============================================================================
void MainComponent::importFiles (const juce::StringArray& files, int trackId, juce::int64 position)
{
    juce::StringArray errors;
    editState.selectedClips.clear();

    for (int i = 0; i < files.size(); ++i)
    {
        const juce::File file (files[i]);
        int target = (i == 0 && session->findTrack (trackId) != nullptr) ? trackId : 0;

        if (target == 0)
        {
            auto* track = session->addTrack (file.getFileNameWithoutExtension());
            if (track == nullptr)
                break;
            target = track->id;
        }

        juce::String error;
        const int clipId = session->importAudioFile (file, target, position, error);
        if (clipId == 0)
            errors.add (error);
        else
            editState.selectedClips.insert (clipId);
        editState.selectedTrack = target;
    }

    editState.cursor = position;
    editState.changed();

    if (! errors.isEmpty())
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (utf8 ("가져오지 못한 파일")).withMessage (errors.joinIntoString ("\n"))
                                          .withButton (utf8 ("확인")).withAssociatedComponent (this), nullptr);
}

void MainComponent::showTrackMenu (int trackId)
{
    auto* track = session->findTrack (trackId);
    if (track == nullptr)
        return;

    editState.selectedTrack = trackId;
    editState.changed();
    const int index = session->indexOfTrack (trackId);

    juce::PopupMenu menu;
    menu.addSectionHeader (track->name);
    menu.addItem (1, utf8 ("이름 바꾸기..."));
    menu.addItem (2, utf8 ("위로 이동"), index > 0);
    menu.addItem (3, utf8 ("아래로 이동"), index + 1 < (int) session->getTracks().size());
    menu.addSeparator();
    menu.addItem (4, utf8 ("트랙 삭제"));

    menu.showMenuAsync ({}, [safe = juce::Component::SafePointer<MainComponent> (this), trackId, index] (int result)
    {
        if (safe == nullptr)
            return;
        switch (result)
        {
            case 1: safe->renameTrackDialog (trackId); break;
            case 2: safe->session->moveTrack (trackId, index - 1); break;
            case 3: safe->session->moveTrack (trackId, index + 1); break;
            case 4: safe->deleteTrack (trackId); break;
            default: break;
        }
    });
}

void MainComponent::renameTrackDialog (int trackId)
{
    auto* track = session->findTrack (trackId);
    if (track == nullptr)
        return;

    textDialog = std::make_unique<juce::AlertWindow> (utf8 ("트랙 이름"), juce::String(), juce::MessageBoxIconType::NoIcon, this);
    textDialog->addTextEditor ("name", track->name);
    textDialog->addButton (utf8 ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    textDialog->addButton (utf8 ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    textDialog->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<MainComponent> (this), trackId] (int result)
        {
            if (safe == nullptr || safe->textDialog == nullptr)
                return;
            if (result == 1)
                safe->session->renameTrack (trackId, safe->textDialog->getTextEditorContents ("name"));
            safe->textDialog.reset();
        }), false);
}

void MainComponent::deleteTrack (int trackId)
{
    auto* track = session->findTrack (trackId);
    if (track == nullptr)
        return;

    auto remove = [safe = juce::Component::SafePointer<MainComponent> (this), trackId]
    {
        if (safe == nullptr)
            return;
        safe->session->removeTrack (trackId);
        safe->editState.selectedTrack = safe->session->getTracks().empty() ? 0 : safe->session->getTracks().front()->id;
        safe->editState.clearSelection();
    };

    if (track->clips.empty() && track->getInserts().getEntries().empty())
    {
        remove();
        return;
    }

    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (utf8 ("트랙 삭제"))
                                      .withMessage (utf8 ("'") + track->name + utf8 ("' 트랙과 그 클립, 플러그인을 지웁니다. 트랙 삭제는 실행 취소할 수 없습니다."))
                                      .withButton (utf8 ("삭제")).withButton (utf8 ("취소")).withAssociatedComponent (this),
                                  [remove] (int result) { if (result == 1) remove(); });
}

//==============================================================================
juce::File MainComponent::defaultSessionFolder() const
{
    const auto last = settings->getValue (kLastFolderKey);
    if (last.isNotEmpty() && juce::File (last).isDirectory())
        return juce::File (last);
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("DAW Sessions");
}

void MainComponent::confirmDiscard (std::function<void()> proceed)
{
    if (! session->isDirty())
    {
        proceed();
        return;
    }

    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (utf8 ("저장하지 않은 변경 사항"))
                                      .withMessage (utf8 ("'") + session->getName() + utf8 ("' 세션의 변경 사항을 저장할까요?"))
                                      .withButton (utf8 ("저장")).withButton (utf8 ("저장 안 함")).withButton (utf8 ("취소"))
                                      .withAssociatedComponent (this),
        [safe = juce::Component::SafePointer<MainComponent> (this), proceed] (int result)
        {
            if (safe == nullptr)
                return;
            if (result == 1)
                safe->saveSession ([proceed] (bool saved) { if (saved) proceed(); });
            else if (result == 2)
                proceed();
        });
}

void MainComponent::requestQuit (std::function<void()> quit)
{
    if (bouncer != nullptr)
        return;
    confirmDiscard (std::move (quit));
}

void MainComponent::newSession()
{
    engine.stop();
    editorWindows.clear();
    session->reset (engine.getSampleRate() > 0.0 ? engine.getSampleRate() : 48000.0);
    session->addTrack();
    session->setDirty (false);
    editState.clearSelection();
    editState.cursor = 0;
    editState.selectedTrack = session->getTracks().front()->id;
    editState.setScroll (0.0);
    deviceChanged();
}

void MainComponent::openSessionChooser()
{
    chooser = std::make_unique<juce::FileChooser> (utf8 ("세션 열기"), defaultSessionFolder(), juce::String ("*") + SessionFile::kExtension);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult() != juce::File())
            safe->openSession (fc.getResult());
    });
}

void MainComponent::openSession (const juce::File& file)
{
    engine.stop();
    editorWindows.clear();
    juce::String error;
    showMessage (utf8 ("세션을 여는 중..."));

    const bool ok = SessionFile::load (*session, file, error,
        [safe = juce::Component::SafePointer<MainComponent> (this)] (juce::StringArray problems)
        {
            if (safe == nullptr)
                return;
            safe->session->setDirty (false);
            if (problems.isEmpty())
                safe->showMessage (utf8 ("세션을 열었습니다."));
            else
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                                  .withTitle (utf8 ("일부를 불러오지 못했습니다"))
                                                  .withMessage (problems.joinIntoString ("\n"))
                                                  .withButton (utf8 ("확인")).withAssociatedComponent (safe.getComponent()), nullptr);
        });

    if (! ok)
    {
        showMessage (error);
        return;
    }

    // Ask the device for the session's rate, so it plays at the right speed.
    const double rate = session->getSampleRate();
    auto& dm = engine.getDeviceManager();
    if (dm.getCurrentAudioDevice() != nullptr
        && ! juce::exactlyEqual (dm.getCurrentAudioDevice()->getCurrentSampleRate(), rate))
    {
        auto setup = dm.getAudioDeviceSetup();
        setup.sampleRate = rate;
        dm.setAudioDeviceSetup (setup, true);
    }

    settings->setValue (kLastFolderKey, file.getParentDirectory().getParentDirectory().getFullPathName());
    editState.clearSelection();
    editState.cursor = 0;
    editState.selectedTrack = session->getTracks().empty() ? 0 : session->getTracks().front()->id;
    editState.sampleRate = session->getSampleRate();
    editState.tempo = session->getTempo();
    engine.locate (0);
    zoomToFit();
    deviceChanged();
}

void MainComponent::saveSession (std::function<void (bool)> then)
{
    if (session->getFile() == juce::File())
    {
        saveSessionAs (std::move (then));
        return;
    }

    const auto error = SessionFile::save (*session, session->getFile());
    if (error.isNotEmpty())
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (utf8 ("저장하지 못했습니다")).withMessage (error)
                                          .withButton (utf8 ("확인")).withAssociatedComponent (this), nullptr);
    else
        showMessage (utf8 ("저장했습니다: ") + session->getFile().getFullPathName());

    if (then)
        then (error.isEmpty());
}

void MainComponent::saveSessionAs (std::function<void (bool)> then)
{
    const auto folder = defaultSessionFolder();
    folder.createDirectory();
    chooser = std::make_unique<juce::FileChooser> (utf8 ("세션 저장 - 세션 이름과 같은 폴더가 만들어집니다"),
                                                   folder.getChildFile (session->getName() + SessionFile::kExtension),
                                                   juce::String ("*") + SessionFile::kExtension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<MainComponent> (this), then] (const juce::FileChooser& fc)
    {
        if (safe == nullptr)
            return;

        auto file = fc.getResult();
        if (file == juce::File())
        {
            if (then) then (false);
            return;
        }

        // Each session lives in its own folder, named after it.
        file = file.withFileExtension (SessionFile::kExtension);
        const auto name = file.getFileNameWithoutExtension();
        if (file.getParentDirectory().getFileName() != name)
            file = file.getParentDirectory().getChildFile (name).getChildFile (file.getFileName());
        file.getParentDirectory().createDirectory();

        safe->settings->setValue (kLastFolderKey, file.getParentDirectory().getParentDirectory().getFullPathName());
        safe->session->setFile (file);
        safe->saveSession (then);
    });
}

void MainComponent::importChooser()
{
    chooser = std::make_unique<juce::FileChooser> (utf8 ("오디오 가져오기"), defaultSessionFolder(), kAudioWildcard);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr)
            return;
        juce::StringArray files;
        for (const auto& f : fc.getResults())
            files.add (f.getFullPathName());
        if (! files.isEmpty())
            safe->importFiles (files, safe->editState.selectedTrack, safe->editState.cursor);
    });
}

void MainComponent::bounceChooser()
{
    const auto start = editState.selection.isEmpty() ? 0 : editState.selection.start;
    const auto end = editState.selection.isEmpty() ? session->getLength() : editState.selection.end;
    if (end <= start)
    {
        showMessage (utf8 ("내보낼 오디오가 없습니다."));
        return;
    }

    bool loading = session->getMasterInserts().isRestoring();
    for (const auto& t : session->getTracks())
        loading = loading || t->getInserts().isRestoring();
    if (loading)
    {
        showMessage (utf8 ("플러그인을 불러오는 중입니다. 끝난 뒤 다시 내보내세요."));
        return;
    }

    const auto folder = session->getFile() != juce::File() ? session->getFile().getParentDirectory() : defaultSessionFolder();
    chooser = std::make_unique<juce::FileChooser> (utf8 ("믹스다운 내보내기"), folder.getChildFile (session->getName() + ".wav"), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<MainComponent> (this), start, end] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;

        auto& self = *safe;
        const auto target = fc.getResult().withFileExtension (".wav");
        self.editorWindows.clear();
        if (self.settingsDialog != nullptr)   // the device must stay detached while rendering
            delete self.settingsDialog.getComponent();
        self.bouncer = std::make_unique<Bouncer> (*self.session, target, start, end);
        self.overlay = std::make_unique<BusyOverlay> ([b = self.bouncer.get()] { return b->getProgress(); },
                                                      [b = self.bouncer.get()] { b->cancel(); });
        self.addAndMakeVisible (*self.overlay);
        self.overlay->setBounds (self.getLocalBounds());

        self.bouncer->start ([safe, target] (const juce::String& error)
        {
            if (safe == nullptr)
                return;
            // Deleting the bouncer inside its own callback is avoided: later.
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->bouncer.reset(); safe->overlay.reset(); } });
            if (error.isEmpty())
                safe->showMessage (utf8 ("내보냈습니다: ") + target.getFullPathName());
            else
                safe->showMessage (error);
        });
    });
}

void MainComponent::showAudioSettings()
{
    if (settingsDialog != nullptr)
    {
        settingsDialog->toFront (true);
        return;
    }

    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(), 0, MixEngine::kMaxInputChannels, 2, 2, false, false, false, false);
    selector->setSize (560, 480);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = utf8 ("오디오 설정");
    options.dialogBackgroundColour = DAWColours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;
    settingsDialog = options.launchAsync();
}

//==============================================================================
void MainComponent::handleCommandLine (const juce::StringArray& args)
{
    juce::File screenshot;
    bool mix = false;
    juce::StringArray audio;

    for (int i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        if (a == "--screenshot" && i + 1 < args.size())
            screenshot = juce::File (args[++i]);
        else if (a == "--mix")
            mix = true;
        else if (a.endsWithIgnoreCase (SessionFile::kExtension))
            openSession (juce::File (a));
        else if (juce::File (a).existsAsFile())
            audio.add (juce::File (a).getFullPathName());
    }

    if (! audio.isEmpty())
    {
        importFiles (audio, editState.selectedTrack, 0);
        zoomToFit();
    }

    if (mix)
        showMix (true);

    if (screenshot != juce::File())
    {
        juce::Timer::callAfterDelay (2500, [safe = juce::Component::SafePointer<MainComponent> (this), screenshot]
        {
            if (safe == nullptr)
                return;
            auto image = safe->createComponentSnapshot (safe->getLocalBounds());
            screenshot.deleteFile();
            juce::FileOutputStream out (screenshot);
            juce::PNGImageFormat().writeImageToStream (image, out);
            out.flush();
            juce::JUCEApplication::getInstance()->quit();
        });
    }
}
