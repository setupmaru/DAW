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
    audioSettingsButton.setTooltip (utf8 ("마이크가 꽂힌 오디오 인터페이스와 입력 채널, 출력을 고릅니다."));
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible (audioSettingsButton);

    scanButton.setTooltip (utf8 ("새로 설치한 VST3 플러그인을 찾습니다. 플러그인이 많으면 시간이 걸릴 수 있습니다."));
    scanButton.onClick = [this] { startScan(); };
    addAndMakeVisible (scanButton);

    inputMeter.setTooltip (utf8 ("마이크로 들어오는 소리 크기입니다. 말할 때 움직이지 않으면 [오디오 설정]에서 입력을 확인하세요."));
    outputMeter.setTooltip (utf8 ("효과를 거친 뒤 내보내는 소리 크기입니다. 빨간색까지 차면 너무 큽니다."));

    addAndMakeVisible (inputMeter);
    addAndMakeVisible (outputMeter);

    // Row clicks act on the chain model by id. An id the model no longer knows
    // (the row is about to be rebuilt) is simply refused by the model.
    chainList.onBypassRequested = [this] (int id, bool bypassed) { chain.setBypassed (id, bypassed); };
    chainList.onEditRequested   = [this] (int id) { openEditor (id); };
    chainList.onMoveRequested   = [this] (int id, int newIndex) { chain.move (id, newIndex); };
    chainList.onRemoveRequested = [this] (int id) { chain.remove (id); };
    chainList.onAddRequested    = [this] { showAddPluginMenu(); };
    addAndMakeVisible (chainList);

    chain.onChanged = [this] { chainChanged(); };
    chain.onError   = [this] (const juce::String& message) { showNotice (message); };

    scanner.loadCache();
    updateScanButton();

    // A failure keeps the app open: the message stays at the top, and a device
    // can still be chosen in the audio settings dialog.
    startupDeviceError = engine.initialise();

    syncWithDevice();
    engine.getDeviceManager().addChangeListener (this);

    chainChanged();
    startTimer (statusTimer, kStatusRefreshMs);
    setSize (640, 480);
}

MainComponent::~MainComponent()
{
    stopTimer (noticeTimer);
    stopTimer (statusTimer);
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
    audioSettingsButton.setBounds (top.removeFromLeft (120));
    // Narrow windows: the meters give up width so the status line stays readable.
    const int meterWidth = juce::jmin (kMeterWidth, (top.getWidth() * 2) / 5);
    inputMeter.setBounds (top.removeFromRight (meterWidth));
    statusLabel.setBounds (top.reduced (6, 0));

    auto bottom = area.removeFromBottom (kBarHeight).reduced (kMargin, 6);
    scanButton.setBounds (bottom.removeFromLeft (160));
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

    const auto full = deviceName + "  ·  " + numbers;
    const bool fits = juce::GlyphArrangement::getStringWidth (statusLabel.getFont(), full)
                      <= (float) statusLabel.getWidth() - 8.0f;

    statusLabel.setText (fits ? full : numbers, juce::dontSendNotification);
    statusLabel.setTooltip (utf8 ("장치: ") + deviceName + "\n"
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
    if (timerId == statusTimer)
    {
        // The chain's total delay changes asynchronously (after fades), so poll it.
        updateStatus();
        return;
    }

    endTimedNotice();
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
                                safe->chain.addPlugin (types.getReference (index));
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
