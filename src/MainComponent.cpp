#include "MainComponent.h"

#include <algorithm>

namespace
{
juce::String utf8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

constexpr int kNoticeMs = 5000;
constexpr int kDeviceChangedNoticeMs = 15000;   // important: leave it up longer
constexpr int kStatusRefreshMs = 500;
constexpr int kAutosaveDelayMs = 2000;          // after a chain change
constexpr int kPeriodicSaveMs = 60 * 1000;      // catches knob changes inside plugin editors
constexpr int kRestoreProblemNoticeMs = 15000;
// The restore pedal stays down this long after the launch restore, so a
// plugin that crashes on its first audio blocks is caught as well.
constexpr int kRestorePedalHoldMs = 5000;

constexpr const char* kMutedSettingKey = "outputMuted";
constexpr const char* kDeviceSettingKey = "audioDevice";   // AudioDeviceManager::createStateXml()
constexpr int kNoticeHeight = 30;
constexpr int kBarHeight = 44;
constexpr int kMeterWidth = 240;
constexpr int kMargin = 8;

// The engine uses one input channel (design: 선택한 입력 채널 1개), so the
// settings dialog allows only one; choosing another switches to it.
constexpr int kMaxInputChannels = 1;
} // namespace

//==============================================================================
MainComponent::MainComponent()
    : scanner (formatManager, PluginScanner::defaultCacheFile()),
      chain (engine.getChain(), formatManager),
      inputMeter (engine.getInputMeter(), utf8 ("입력")),
      outputMeter (engine.getOutputMeter(), utf8 ("출력"))
{
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());

    noticeLabel.setJustificationType (juce::Justification::centredLeft);
    noticeLabel.setMinimumHorizontalScale (0.6f);
    noticeLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff8a4b00));
    noticeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addChildComponent (noticeLabel);

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setMinimumHorizontalScale (0.8f);
    statusLabel.setFont (juce::FontOptions (13.0f));
    statusLabel.setColour (juce::Label::textColourId,
                           getLookAndFeel().findColour (juce::Label::textColourId).withAlpha (0.7f));
    addAndMakeVisible (statusLabel);

    audioSettingsButton.setButtonText (utf8 ("오디오 설정"));
    audioSettingsButton.setTooltip (utf8 ("마이크가 꽂힌 오디오 인터페이스와 입력 채널, 출력을 고릅니다. ")
                                    + utf8 ("인터페이스 전용 ASIO 드라이버가 있으면 장치 종류에서 ASIO를 고르면 지연이 가장 짧습니다. 고른 장치는 다음 실행 때도 그대로 열립니다."));
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible (audioSettingsButton);

    presetButton.setButtonText (utf8 ("프리셋 ▼"));
    presetButton.setTooltip (utf8 ("지금 체인을 이름 붙여 저장하거나, 저장해 둔 체인으로 바꿉니다."));
    presetButton.onClick = [this] { showPresetMenu(); };
    addAndMakeVisible (presetButton);

    muteButton.setColour (juce::TextButton::buttonColourId, juce::Colours::seagreen.darker (0.3f));
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::firebrick);
    muteButton.onClick = [this] { setMuted (! engine.isOutputMuted()); };
    // Return/space must not toggle the broadcast output after a click on it.
    muteButton.setWantsKeyboardFocus (false);
    addAndMakeVisible (muteButton);

    scanButton.setTooltip (utf8 ("새로 설치한 VST3 플러그인을 찾습니다. 플러그인이 많으면 시간이 걸릴 수 있습니다."));
    scanButton.onClick = [this] { startScan(); };
    addAndMakeVisible (scanButton);

    inputMeter.setTooltip (utf8 ("마이크로 들어오는 소리 크기입니다. 말할 때 움직이지 않으면 [오디오 설정]에서 입력을 확인하세요."));
    outputMeter.setTooltip (utf8 ("실제로 내보내는 소리 크기입니다(음소거 중이면 움직이지 않습니다). 빨간색까지 차면 너무 큽니다."));

    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);

    // Row clicks act on the chain model by id. An id the model no longer knows
    // (the row is about to be rebuilt) is simply refused by the model.
    // These four are the user's own edits: they release the autosave hold that
    // protects a partly failed launch restore (AutosaveGate).
    chainList.onBypassRequested = [this] (int id, bool bypassed) { if (chain.setBypassed (id, bypassed)) autosaveGate.userEdited(); };
    chainList.onEditRequested   = [this] (int id) { openEditor (id); };
    chainList.onMoveRequested   = [this] (int id, int newIndex) { if (chain.move (id, newIndex)) autosaveGate.userEdited(); };
    chainList.onRemoveRequested = [this] (int id) { if (chain.remove (id)) autosaveGate.userEdited(); };
    chainList.onAddRequested    = [this] { showAddPluginMenu(); };
    addAndMakeVisible (chainList);

    chain.onChanged = [this] { chainChanged(); };
    chain.onError   = [this] (const juce::String& message) { showNotice (message); };

    scanner.loadCache();
    updateScanButton();

    // The remembered mute state, set before the device opens so the very first
    // block already follows it (no burst of sound before muting).
    const bool startMuted = store.getSettings().getBoolValue (kMutedSettingKey, false);
    engine.setOutputMuted (startMuted);
    updateMuteButton();

    // A failure keeps the app open: the message stays at the top, and a device
    // can still be chosen in the audio settings dialog.
    // The device chosen last time (type such as ASIO, devices, rate, buffer),
    // or the default device on the first run or when it is gone.
    const auto savedDevice = store.getSettings().getXmlValue (kDeviceSettingKey);
    startupDeviceError = engine.initialise (savedDevice.get());

    // The saved device could not be opened and another one was (interface
    // unplugged, driver removed): say so, or the mic may silently be the
    // laptop's. Shown below, after the startup notices are settled.
    juce::String fallbackNotice;

    if (savedDevice != nullptr)
    {
        const auto savedName = savedDevice->getStringAttribute ("audioOutputDeviceName");
        auto* opened = engine.getDeviceManager().getCurrentAudioDevice();

        if (savedName.isNotEmpty() && opened != nullptr && opened->getName() != savedName)
            fallbackNotice = utf8 ("저장해 둔 오디오 장치를 찾지 못해 다른 장치로 열었습니다: ") + savedName + utf8 (" → ")
                             + opened->getName() + utf8 (". 오디오 인터페이스가 연결돼 있는지 확인하세요.");
    }

    syncWithDevice();
    engine.getDeviceManager().addChangeListener (this);

    chainChanged();
    restoreLastChain();   // works with or without a device

    // Starting muted on purpose is fine, but it must not go unnoticed: a
    // streamer could go live with no sound. (A device or restore problem shown
    // already takes precedence; the red button still shows the state.)
    if (fallbackNotice.isNotEmpty())
        showNotice (fallbackNotice, false, kDeviceChangedNoticeMs);
    else if (startMuted && ! noticeLabel.isVisible())
        showNotice (utf8 ("지난번에 음소거해 둔 상태로 시작했습니다. 방송에 소리를 내보내려면 [음소거 중]을 누르세요."),
                    false, kRestoreProblemNoticeMs);

    startTimer (statusTimer, kStatusRefreshMs);
    startTimer (periodicSaveTimer, kPeriodicSaveMs);
    setSize (640, 480);
}

MainComponent::~MainComponent()
{
    stopTimer (noticeTimer);
    stopTimer (statusTimer);
    stopTimer (autosaveTimer);
    stopTimer (periodicSaveTimer);
    const bool pedalPending = isTimerRunning (restorePedalTimer);
    stopTimer (restorePedalTimer);

    // Quitting during (or just after) the launch restore is not a crash:
    // release the pedal so the next launch restores normally.
    if (chain.isRestoring() || pedalPending)
        store.releaseRestorePedal();

    // Save the chain while every plugin is still alive (their state is read
    // now). Skipped mid-restore and while a partly failed launch restore is
    // protected (AutosaveGate). Silent: nobody would see a notice now.
    saveLastChainNow (false);

    presetNameDialog.reset();
    engine.getDeviceManager().removeChangeListener (this);

    // The selector inside holds a reference to the device manager.
    if (settingsDialog != nullptr)
        delete settingsDialog.getComponent();

    // Defensive: the chain must never call back into a half-destroyed
    // component. (Its destructor makes no callbacks, and everything here runs
    // synchronously on the message thread, so none is expected.)
    chain.onChanged = nullptr;
    chain.onError = nullptr;

    // Editor windows first, explicitly, before any processor can go away. The
    // remaining members are then destroyed in reverse declaration order (see
    // the header): chain, scanner, engine (shuts the device down), format
    // manager.
    editorWindows.clear();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    // Separator lines above and below the chain list.
    g.setColour (getLookAndFeel().findColour (juce::Label::textColourId).withAlpha (0.25f));
    g.fillRect (0, chainList.getY() - 1, getWidth(), 1);
    g.fillRect (0, chainList.getBottom(), getWidth(), 1);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // The notice strip's height is always reserved, so the chain rows never
    // move when a notice shows or hides (no mis-clicks on the wrong row).
    noticeLabel.setBounds (area.removeFromTop (kNoticeHeight));

    auto top = area.removeFromTop (kBarHeight).reduced (kMargin, 6);
    audioSettingsButton.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (6);
    presetButton.setBounds (top.removeFromLeft (92));
    // Narrow windows: the meters give up width so the status line stays readable.
    const int meterWidth = juce::jmin (kMeterWidth, (top.getWidth() * 2) / 5);
    inputMeter.setBounds (top.removeFromRight (meterWidth));
    statusLabel.setBounds (top.reduced (6, 0));

    auto bottom = area.removeFromBottom (kBarHeight).reduced (kMargin, 6);
    scanButton.setBounds (bottom.removeFromLeft (150));
    bottom.removeFromLeft (8);
    muteButton.setBounds (bottom.removeFromLeft (110));
    outputMeter.setBounds (bottom.removeFromRight (meterWidth));

    chainList.setBounds (area.reduced (0, 1));
    updateStatus();   // the device name is shown only when the line fits
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The device manager changed (device, sample rate, block size or channels),
    // usually from the audio settings dialog.
    syncWithDevice();

    // Remember the user's choice for the next launch. createStateXml() holds
    // only explicitly chosen settings, so JUCE falling back to the default
    // device (interface unplugged) does not overwrite the saved choice.
    if (const auto state = engine.getDeviceManager().createStateXml())
    {
        store.getSettings().setValue (kDeviceSettingKey, state.get());
        store.saveSettings();
    }
}

void MainComponent::syncWithDevice()
{
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    // A change made in the settings dialog is the user's own choice; anything
    // else (interface unplugged, driver fallback to the default device) is not.
    const bool userIsChoosing = settingsDialog != nullptr;

    if (device == nullptr)
    {
        if (lastDeviceName.isNotEmpty() && ! userIsChoosing)
        {
            showNotice (utf8 ("오디오 장치 연결이 끊겼습니다: ") + lastDeviceName
                            + utf8 (". 마이크 소리가 나가지 않습니다. 오디오 인터페이스 연결을 확인하거나 [오디오 설정]에서 장치를 고르세요."),
                        true);
        }
        else
        {
            // "None" picked on purpose in the dialog: forget the old device, so a
            // device opening later is not reported as an unexpected switch, and
            // replace any disconnect message with the plain hint.
            if (userIsChoosing)
                lastDeviceName.clear();

            if (userIsChoosing || stickyNoticeText.isEmpty())
                showNoDeviceNotice();
        }

        updateStatus();
        return;
    }

    const auto name = device->getName();
    const bool switchedBehindUsersBack = lastDeviceName.isNotEmpty() && name != lastDeviceName && ! userIsChoosing;
    const auto previousName = lastDeviceName;
    lastDeviceName = name;
    startupDeviceError.clear();   // a device is open now

    if (switchedBehindUsersBack)
    {
        stickyNoticeText.clear();   // a device is open; the switch notice replaces any disconnect message
        showNotice (utf8 ("오디오 장치가 바뀌었습니다: ") + previousName + utf8 (" → ") + name
                        + utf8 (". 마이크 소리가 안 나올 수 있습니다. 오디오 인터페이스 연결을 확인하고, [오디오 설정]에서 장치를 다시 고르세요."),
                    false, kDeviceChangedNoticeMs);
    }
    else if (stickyNoticeText.isNotEmpty())
    {
        clearNotice();   // the device problem is solved
    }

    // New plugin slots must be prepared like the ChainProcessor, which the
    // engine prepares from audioDeviceAboutToStart with these same values.
    const double sampleRate = device->getCurrentSampleRate();
    const int blockSize = device->getCurrentBufferSizeSamples();

    if (sampleRate > 0.0 && blockSize > 0)
        chain.setProcessingSpec (sampleRate, blockSize);

    // Input channel: the engine uses the first active input channel of the
    // device. The device hands the IO callback only its active channels,
    // packed in order, so that channel is index 0 in the callback. With no
    // active input, keep 0 as well: the callback then has no channel 0 and the
    // engine outputs silence.
    // (The device's own channel number, getActiveInputChannels().findNextSetBit
    // (0), would be wrong here whenever a channel other than the first is
    // chosen.)
    engine.setInputChannel (0);

    // A new sample rate re-prepares the plugins, which can change their delay
    // in ms: rebuild the rows (asynchronously) and the status line.
    chainChanged();
}

void MainComponent::showNoDeviceNotice()
{
    // Keep the specific reason from launch (e.g. the driver's own message) as
    // long as no device has opened; fall back to the generic hint otherwise.
    showNotice (startupDeviceError.isNotEmpty()
                    ? startupDeviceError
                    : utf8 ("열린 오디오 장치가 없습니다. [오디오 설정]에서 장치를 고르세요."),
                true);
}

void MainComponent::updateStatus()
{
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();

    if (device == nullptr)
    {
        statusLabel.setText (utf8 ("장치 없음"), juce::dontSendNotification);
        statusLabel.setTooltip ({});
        return;
    }

    const double sampleRate = device->getCurrentSampleRate();
    const int bufferSize = device->getCurrentBufferSizeSamples();
    const auto deviceName = device->getName();

    if (sampleRate <= 0.0)
    {
        statusLabel.setText (deviceName, juce::dontSendNotification);
        statusLabel.setTooltip ({});
        return;
    }

    const double bufferMs = 1000.0 * bufferSize / sampleRate;
    const double chainMs = 1000.0 * engine.getChain().getTotalLatencySamples() / sampleRate;

    // The numbers matter most, so they come first and stay readable; the device
    // name is added in front only when the whole line fits.
    auto numbers = juce::String (sampleRate / 1000.0, 1) + "kHz  ·  " + utf8 ("버퍼 ") + ui::formatMs (bufferMs);

    if (chainMs >= ui::kMinShownLatencyMs)
        numbers << utf8 ("  ·  효과 지연 ") << ui::formatMs (chainMs);

    const auto typeName = engine.getDeviceManager().getCurrentAudioDeviceType();
    const auto full = deviceName + (typeName.isNotEmpty() ? " (" + typeName + ")" : juce::String()) + "  ·  " + numbers;
    const bool fits = juce::GlyphArrangement::getStringWidth (statusLabel.getFont(), full)
                      <= (float) statusLabel.getWidth() - 8.0f;

    auto line = fits ? full : numbers;

    // Kept visible even when a notice covers the strip above.
    if (engine.isOutputMuted())
        line = utf8 ("음소거 중  ·  ") + line;

    statusLabel.setText (line, juce::dontSendNotification);
    statusLabel.setTooltip (utf8 ("장치: ") + deviceName + "\n"
                            + (typeName.isNotEmpty() ? utf8 ("종류: ") + typeName + "\n" : juce::String())
                            + utf8 ("버퍼가 작을수록 목소리가 덜 늦게 나가지만, 소리가 끊기면 [오디오 설정]에서 버퍼를 키우세요.\n")
                            + utf8 ("효과 지연은 체인의 플러그인이 더하는 지연입니다."));
}

//==============================================================================
void MainComponent::showNotice (const juce::String& text, bool sticky, int durationMs)
{
    noticeLabel.setText (text, juce::dontSendNotification);
    noticeLabel.setTooltip (text);   // long messages are squeezed; hover shows them whole
    noticeLabel.setVisible (true);

    if (sticky)
    {
        stickyNoticeText = text;
        stopTimer (noticeTimer);
    }
    else
    {
        // Covers a sticky device message for a while; endTimedNotice brings it back.
        startTimer (noticeTimer, durationMs > 0 ? durationMs : kNoticeMs);   // the latest message gets the full time
    }
}

void MainComponent::clearNotice()
{
    stopTimer (noticeTimer);
    stickyNoticeText.clear();
    noticeLabel.setText ({}, juce::dontSendNotification);
    noticeLabel.setTooltip ({});
    noticeLabel.setVisible (false);
}

void MainComponent::endTimedNotice()
{
    stopTimer (noticeTimer);

    if (stickyNoticeText.isNotEmpty())
        showNotice (stickyNoticeText, true);   // the device problem is still there
    else
        clearNotice();
}

void MainComponent::timerCallback (int timerId)
{
    switch (timerId)
    {
        case statusTimer:
            // The chain's total delay changes asynchronously (after fades), so poll it.
            updateStatus();
            updateEditingEnabled();
            return;

        case autosaveTimer:
            stopTimer (autosaveTimer);   // one-shot
            saveLastChainNow();
            return;

        case periodicSaveTimer:
            saveLastChainNow();
            return;

        case restorePedalTimer:
            stopTimer (restorePedalTimer);   // one-shot
            store.releaseRestorePedal();      // the restored chain has been running fine
            return;

        default:
            endTimedNotice();
            return;
    }
}

//==============================================================================
void MainComponent::chainChanged()
{
    const auto& entries = chain.getEntries();

    // Delete the editor windows of entries that left the model. This runs
    // synchronously from the chain, before a dropped entry's processor can be
    // deleted (removal and the "ghost" path alike).
    for (auto it = editorWindows.begin(); it != editorWindows.end();)
    {
        const int id = it->first;
        const bool stillThere = std::any_of (entries.begin(), entries.end(),
                                             [id] (const ChainEntry& e) { return e.id == id; });
        it = stillThere ? std::next (it) : editorWindows.erase (it);
    }

    std::vector<ChainListComponent::RowData> rows;
    rows.reserve (entries.size());

    for (const auto& e : entries)
    {
        double latencyMs = 0.0;

        if (e.processor != nullptr)
        {
            const double sampleRate = e.processor->getSampleRate();

            if (sampleRate > 0.0)
                latencyMs = 1000.0 * e.processor->getLatencySamples() / sampleRate;
        }

        rows.push_back ({ e.id, e.name, e.bypassed, e.processor != nullptr && e.processor->hasEditor(), latencyMs });
    }

    chainList.setRows (std::move (rows));
    updateStatus();
    updateEditingEnabled();

    scheduleAutosave();
}

void MainComponent::openEditor (int id)
{
    if (auto it = editorWindows.find (id); it != editorWindows.end())
    {
        it->second->setVisible (true);
        it->second->toFront (true);
        return;
    }

    const auto& entries = chain.getEntries();
    const auto entry = std::find_if (entries.begin(), entries.end(),
                                     [id] (const ChainEntry& e) { return e.id == id; });

    if (entry == entries.end() || entry->processor == nullptr)
        return;

    if (! entry->processor->hasEditor())
    {
        showNotice (utf8 ("이 플러그인은 편집 화면이 없습니다: ") + entry->name);
        return;
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (entry->processor->createEditorAndMakeActive());

    if (editor == nullptr)
    {
        showNotice (utf8 ("편집 창을 열 수 없습니다: ") + entry->name);
        return;
    }

    // Closing the window deletes only the window; the plugin stays in the chain.
    editorWindows[id] = std::make_unique<PluginEditorWindow> (
        entry->name, std::move (editor), [this, id] { closeEditor (id); });
}

void MainComponent::closeEditor (int id)
{
    editorWindows.erase (id);
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    if (settingsDialog != nullptr)
    {
        settingsDialog->toFront (true);
        return;
    }

    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(),
        0, kMaxInputChannels,   // inputs
        2, 2,                   // outputs: stereo
        false, false,           // no MIDI
        false,                  // inputs as single channels
        false);                 // show the advanced options
    selector->setSize (520, 460);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = utf8 ("오디오 설정");
    options.dialogBackgroundColour = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;

    // launchAsync shows the dialog asynchronously modal (it does not block
    // here) and the dialog deletes itself when closed.
    settingsDialog = options.launchAsync();
}

void MainComponent::showAddPluginMenu()
{
    // A copy, so the menu result maps onto the list the menu was built from
    // even if a scan changes the known plugins while the menu is open.
    auto types = scanner.getKnownPlugins().getTypes();

    // Instruments have no audio input and would only be refused when added.
    types.removeIf ([] (const juce::PluginDescription& d) { return d.isInstrument; });

    juce::PopupMenu menu;

    if (types.isEmpty())
        menu.addItem (1, utf8 ("스캔된 플러그인이 없습니다. 먼저 스캔하세요."), false);
    else
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortAlphabetically);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chainList.getAddButton()),
                        [safe = juce::Component::SafePointer<MainComponent> (this), types] (int result)
                        {
                            if (safe == nullptr || result == 0 || types.isEmpty())
                                return;

                            const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

                            if (juce::isPositiveAndBelow (index, types.size()))
                            {
                                safe->autosaveGate.userEdited();
                                safe->chain.addPlugin (types.getReference (index));
                            }
                        });
}

//==============================================================================
void MainComponent::startScan()
{
    if (scanner.isScanning())
        return;

    // The add-plugin menu is built from the scanner's list each time it opens,
    // so the finished scan is picked up there without anything else to do.
    scanner.startScan ([safe = juce::Component::SafePointer<MainComponent> (this)]
                       {
                           if (safe == nullptr)
                               return;

                           safe->updateScanButton();
                           safe->showNotice (utf8 ("스캔이 끝났습니다. 찾은 플러그인: ")
                                             + juce::String (safe->scanner.getKnownPlugins().getNumTypes())
                                             + utf8 ("개"));
                       });
    updateScanButton();
}

void MainComponent::updateScanButton()
{
    const bool scanning = scanner.isScanning();
    scanButton.setEnabled (! scanning);
    scanButton.setButtonText (scanning ? utf8 ("스캔 중…") : utf8 ("플러그인 다시 스캔"));
}

//==============================================================================
// Output mute

void MainComponent::setMuted (bool muted)
{
    engine.setOutputMuted (muted);   // the engine fades, so this never clicks
    updateMuteButton();
    updateStatus();

    // Remembered at once: the next launch starts in the same state.
    store.getSettings().setValue (kMutedSettingKey, muted);

    if (! store.saveSettings())
        showNotice (utf8 ("음소거 상태를 저장하지 못했습니다. 다음 실행 때는 이전 상태로 시작할 수 있습니다."));
}

void MainComponent::updateMuteButton()
{
    const bool muted = engine.isOutputMuted();
    muteButton.setToggleState (muted, juce::dontSendNotification);   // the "on" colour is the red one
    muteButton.setButtonText (muted ? utf8 ("음소거 중") : utf8 ("출력 켜짐"));
    muteButton.setTooltip (muted
                               ? utf8 ("지금은 소리가 밖으로 나가지 않습니다. 눌러서 출력을 켭니다.")
                               : utf8 ("눌러서 출력을 끕니다(음소거). 효과는 계속 처리되고, 다음 실행 때도 이 상태로 시작합니다."));
}

//==============================================================================
// Presets

namespace
{
// What PluginChain::restoreFromXml accepts at the top level. Checked here as
// well, so a rejected file is never then announced as a successful load.
bool isChainDocument (const juce::XmlElement& xml)
{
    return xml.hasTagName ("DAWChain") && xml.getStringAttribute ("version") == "1";
}
} // namespace

void MainComponent::showPresetMenu()
{
    if (chain.isRestoring())
        return;

    const auto names = store.getPresetNames();   // a copy: the menu result maps onto it

    enum { saveItem = 1, loadBase = 1000, deleteBase = 2000 };

    juce::PopupMenu menu;
    menu.addItem (saveItem, utf8 ("지금 체인을 프리셋으로 저장…"));
    menu.addSeparator();

    if (names.isEmpty())
    {
        menu.addItem (-1, utf8 ("저장된 프리셋이 없습니다"), false);
    }
    else
    {
        juce::PopupMenu loadMenu, deleteMenu;

        for (int i = 0; i < names.size(); ++i)
        {
            loadMenu.addItem (loadBase + i, names[i]);
            deleteMenu.addItem (deleteBase + i, names[i]);
        }

        menu.addSubMenu (utf8 ("불러오기"), loadMenu);
        menu.addSubMenu (utf8 ("삭제"), deleteMenu);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton),
                        [safe = juce::Component::SafePointer<MainComponent> (this), names] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            if (result == saveItem)
                                safe->askPresetNameAndSave();
                            else if (juce::isPositiveAndBelow (result - loadBase, names.size()))
                                safe->confirmAndLoadPreset (names[result - loadBase]);
                            else if (juce::isPositiveAndBelow (result - deleteBase, names.size()))
                                safe->confirmAndDeletePreset (names[result - deleteBase]);
                        });
}

void MainComponent::askPresetNameAndSave()
{
    // A new dialog replaces any earlier (already dismissed) one.
    presetNameDialog = std::make_unique<juce::AlertWindow> (utf8 ("프리셋 저장"),
                                                            utf8 ("지금 체인을 어떤 이름으로 저장할까요?"),
                                                            juce::MessageBoxIconType::NoIcon, this);
    presetNameDialog->addTextEditor ("name", {}, utf8 ("이름"));

    if (auto* editor = presetNameDialog->getTextEditor ("name"))
        editor->setInputRestrictions (PresetStore::kMaxNameLength);

    presetNameDialog->addButton (utf8 ("저장"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    presetNameDialog->addButton (utf8 ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

    presetNameDialog->enterModalState (
        true,
        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this),
                                              dialog = juce::Component::SafePointer<juce::AlertWindow> (presetNameDialog.get())] (int result)
        {
            // This dialog, not whichever one is current by now.
            if (safe == nullptr || dialog == nullptr)
                return;

            // Hidden, not deleted, inside its own callback; the next dialog or
            // the destructor deletes it.
            dialog->setVisible (false);

            if (result == 1)
                safe->savePreset (dialog->getTextEditorContents ("name"));
        }),
        false);
}

void MainComponent::savePreset (const juce::String& typedName)
{
    const auto name = store.toStoredName (typedName);

    if (name.isEmpty())
    {
        showNotice (utf8 ("프리셋 이름을 입력하세요. 파일 이름에 쓸 수 없는 일부 특수문자는 빠집니다."));
        return;
    }

    auto write = [safe = juce::Component::SafePointer<MainComponent> (this), name]
    {
        if (safe == nullptr)
            return;

        if (safe->chain.isRestoring())
        {
            safe->showNotice (utf8 ("체인을 불러오는 중에는 저장할 수 없습니다. 잠시 후 다시 해 보세요."));
            return;
        }

        const auto state = safe->chain.createStateXml();

        if (state != nullptr && safe->store.savePreset (name, *state))
            safe->showNotice (utf8 ("프리셋을 저장했습니다: ") + name);
        else
            safe->showNotice (utf8 ("프리셋을 저장하지 못했습니다: ") + name
                              + utf8 (". 디스크 공간이나 폴더 권한을 확인하세요."));
    };

    if (! store.presetExists (name))
    {
        write();
        return;
    }

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon,
                                                      utf8 ("프리셋 덮어쓰기"),
                                                      utf8 ("같은 이름의 프리셋이 이미 있습니다. 지금 체인으로 덮어쓸까요?\n'")
                                                          + name + "'",
                                                      utf8 ("덮어쓰기"), utf8 ("취소"), this),
        [write] (int result)
        {
            if (result == 1)
                write();
        });
}

void MainComponent::confirmAndLoadPreset (const juce::String& name)
{
    auto load = [safe = juce::Component::SafePointer<MainComponent> (this), name]
    {
        if (safe == nullptr || safe->chain.isRestoring())
            return;

        const auto state = safe->store.loadPreset (name);

        if (state == nullptr || ! isChainDocument (*state))
        {
            safe->showNotice (utf8 ("이 프리셋 파일은 읽을 수 없거나 이 버전에서 쓸 수 없습니다: ") + name);
            return;
        }

        safe->restoreChain (*state, utf8 ("프리셋 '") + name + "'", RestoreKind::preset);
    };

    // Nothing to lose: load straight away.
    if (chain.getEntries().empty())
    {
        load();
        return;
    }

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon,
                                                      utf8 ("프리셋 불러오기"),
                                                      utf8 ("지금 체인을 이 프리셋으로 바꿀까요?\n'") + name + "'\n\n"
                                                          + utf8 ("지금 체인은 사라집니다. 남겨 두려면 먼저 프리셋으로 저장하세요. ")
                                                          + utf8 ("바뀌는 동안 잠깐 효과 없는 소리가 나갈 수 있습니다."),
                                                      utf8 ("바꾸기"), utf8 ("취소"), this),
        [load] (int result)
        {
            if (result == 1)
                load();
        });
}

void MainComponent::confirmAndDeletePreset (const juce::String& name)
{
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon,
                                                      utf8 ("프리셋 삭제"),
                                                      utf8 ("이 프리셋을 지울까요? 되돌릴 수 없습니다.\n'") + name + "'",
                                                      utf8 ("삭제"), utf8 ("취소"), this),
        [safe = juce::Component::SafePointer<MainComponent> (this), name] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            if (safe->store.deletePreset (name))
                safe->showNotice (utf8 ("프리셋을 지웠습니다: ") + name);
            else
                safe->showNotice (utf8 ("프리셋을 지우지 못했습니다: ") + name);
        });
}

//==============================================================================
// Restoring a chain (preset or the last chain)

void MainComponent::restoreChain (const juce::XmlElement& state, const juce::String& what, RestoreKind kind)
{
    // The top level was checked by the caller (isChainDocument). A bad entry
    // inside still makes the chain reject the whole file: it reports an error
    // (onError → notice) and finishes with nothing restored and nothing
    // failed. Known up front, so that case is not announced as a success.
    const int entriesInFile = state.getNumChildElements();

    chain.restoreFromXml (state, [safe = juce::Component::SafePointer<MainComponent> (this),
                                  what, kind, entriesInFile] (PluginChain::RestoreResult result)
    {
        if (safe == nullptr)
            return;

        safe->updateEditingEnabled();

        const bool rejected = result.restored == 0 && result.failedNames.isEmpty() && entriesInFile > 0;
        const bool clean = ! rejected && result.failedNames.isEmpty();

        if (kind == RestoreKind::launch)
        {
            // Finished without crashing; released a little later, once the
            // restored plugins have run (see kRestorePedalHoldMs).
            safe->startTimer (restorePedalTimer, kRestorePedalHoldMs);

            if (rejected)
            {
                // Not retried at every launch, but kept. If it cannot be moved,
                // hold saves so the file is not overwritten.
                safe->autosaveGate.launchRestoreFinished (safe->store.setLastChainAside());
                safe->showNotice (utf8 ("지난번 체인을 이 버전에서 불러올 수 없어 빈 체인으로 시작합니다."),
                                  false, kRestoreProblemNoticeMs);
                return;
            }

            if (clean)
                safe->store.backupLastChain();   // the backup is always a complete chain

            // Not clean: autosave holds until the user edits, so the missing
            // plugins stay in last-chain.xml for the next launch.
            safe->autosaveGate.launchRestoreFinished (clean);
        }
        else if (! rejected)
        {
            safe->autosaveGate.userEdited();   // a preset load is the user's choice of chain
        }

        safe->scheduleAutosave();

        if (! result.failedNames.isEmpty())
        {
            auto text = what + utf8 ("에서 빠진 플러그인: ") + result.failedNames.joinIntoString (", ")
                      + utf8 (". 설치돼 있는지 확인하세요(체인은 16개까지만 들어갑니다).");

            if (kind == RestoreKind::launch)
                text << utf8 (" 체인을 고치지 않고 껐다 켜면 설치한 뒤 다시 불러옵니다. 체인을 고치면 빠진 플러그인은 저장에서도 빠집니다.");

            safe->showNotice (text, false, kRestoreProblemNoticeMs);
            return;
        }

        if (! rejected && kind == RestoreKind::preset)
            safe->showNotice (utf8 ("불러왔습니다: ") + what + utf8 (" (플러그인 ")
                              + juce::String (result.restored) + utf8 ("개)"));
    });

    updateEditingEnabled();   // the chain is restoring now: lock the list
}

void MainComponent::restoreLastChain()
{
    // The launch restore crashed last time (the pedal is still down): do not
    // try again, or the app would crash at every launch.
    if (store.restorePedalIsDown())
    {
        store.releaseRestorePedal();

        // If the file cannot be moved aside, hold saves rather than overwrite it.
        if (store.setLastChainAside())
            autosaveGate.nothingToRestore();
        else
            autosaveGate.launchRestoreFinished (false);

        showNotice (utf8 ("지난번 실행 때 체인을 불러오다가 앱이 멈췄습니다. 문제를 일으킨 플러그인이 있을 수 있어 빈 체인으로 시작합니다."),
                    false, kRestoreProblemNoticeMs);
        return;
    }

    if (! store.hasLastChain())
    {
        autosaveGate.nothingToRestore();
        return;
    }

    const auto state = store.loadLastChain();

    if (state == nullptr || ! isChainDocument (*state))
    {
        // Kept aside (last-chain.broken.xml), so starting empty loses nothing.
        // If it cannot be moved, hold saves rather than overwrite it.
        if (store.setLastChainAside())
            autosaveGate.nothingToRestore();
        else
            autosaveGate.launchRestoreFinished (false);

        showNotice (utf8 ("지난번 체인 파일을 읽을 수 없어 빈 체인으로 시작합니다."), false, kRestoreProblemNoticeMs);
        return;
    }

    autosaveGate.launchRestoreStarted();
    store.pressRestorePedal();
    restoreChain (*state, utf8 ("지난번 체인"), RestoreKind::launch);
}

void MainComponent::scheduleAutosave()
{
    startTimer (autosaveTimer, kAutosaveDelayMs);   // restarts: saves 2 s after the last change
}

void MainComponent::saveLastChainNow (bool reportErrors)
{
    // See AutosaveGate: never before the launch restore has finished, never
    // mid-restore, and not while a partly failed launch restore is protected.
    if (! autosaveGate.savesAllowed (chain.isRestoring()))
        return;

    const auto state = chain.createStateXml();
    const bool saved = state != nullptr && store.saveLastChain (*state);

    // Reported once, not every minute.
    if (! saved && ! autosaveErrorShown && reportErrors)
        showNotice (utf8 ("체인을 자동 저장하지 못했습니다. 디스크 공간이나 폴더 권한을 확인하세요."));

    autosaveErrorShown = ! saved;
}

void MainComponent::updateEditingEnabled()
{
    const bool restoring = chain.isRestoring();

    chainList.setEditingEnabled (! restoring);
    presetButton.setEnabled (! restoring);
    presetButton.setButtonText (restoring ? utf8 ("불러오는 중…") : utf8 ("프리셋 ▼"));
}
