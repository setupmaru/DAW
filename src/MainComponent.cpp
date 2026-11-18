#include "MainComponent.h"

#include <algorithm>

namespace
{
juce::String utf8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

constexpr int kNoticeMs = 5000;
constexpr int kNoticeHeight = 30;
constexpr int kBarHeight = 44;
constexpr int kMeterWidth = 240;
constexpr int kMargin = 8;

// The settings dialog may enable up to this many inputs; the engine uses the
// first enabled one.
constexpr int kMaxInputChannels = 256;
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

    audioSettingsButton.setButtonText (utf8 ("오디오 설정"));
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible (audioSettingsButton);

    scanButton.onClick = [this] { startScan(); };
    addAndMakeVisible (scanButton);

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
    const auto error = engine.initialise();

    if (error.isNotEmpty())
        showNotice (error, true);

    syncWithDevice();
    engine.getDeviceManager().addChangeListener (this);

    chainChanged();
    setSize (640, 480);
}

MainComponent::~MainComponent()
{
    stopTimer();
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

    if (noticeLabel.isVisible())
        noticeLabel.setBounds (area.removeFromTop (kNoticeHeight));

    auto top = area.removeFromTop (kBarHeight).reduced (kMargin, 6);
    audioSettingsButton.setBounds (top.removeFromLeft (120));
    inputMeter.setBounds (top.removeFromRight (kMeterWidth));

    auto bottom = area.removeFromBottom (kBarHeight).reduced (kMargin, 6);
    scanButton.setBounds (bottom.removeFromLeft (160));
    outputMeter.setBounds (bottom.removeFromRight (kMeterWidth));

    chainList.setBounds (area.reduced (0, 1));
    repaint();
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

    if (device == nullptr)
    {
        if (! noticeSticky)
            showNotice (utf8 ("열린 오디오 장치가 없습니다. [오디오 설정]에서 장치를 고르세요."), true);

        return;
    }

    if (noticeSticky)
        clearNotice();   // the device problem is solved

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
}

//==============================================================================
void MainComponent::showNotice (const juce::String& text, bool sticky)
{
    noticeSticky = sticky;
    noticeLabel.setText (text, juce::dontSendNotification);

    if (! noticeLabel.isVisible())
    {
        noticeLabel.setVisible (true);
        resized();
    }

    if (sticky)
        stopTimer();
    else
        startTimer (kNoticeMs);   // restarts: the latest message gets the full time
}

void MainComponent::clearNotice()
{
    stopTimer();
    noticeSticky = false;
    noticeLabel.setText ({}, juce::dontSendNotification);

    if (noticeLabel.isVisible())
    {
        noticeLabel.setVisible (false);
        resized();
    }
}

void MainComponent::timerCallback()
{
    clearNotice();

    // Bring back the device hint if there is still no device.
    if (engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
        syncWithDevice();
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
        rows.push_back ({ e.id, e.name, e.bypassed, e.processor != nullptr && e.processor->hasEditor() });

    chainList.setRows (std::move (rows));
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

    // Not modal: the dialog deletes itself when closed (launchAsync).
    settingsDialog = options.launchAsync();
}

void MainComponent::showAddPluginMenu()
{
    // A copy, so the menu result maps onto the list the menu was built from
    // even if a scan changes the known plugins while the menu is open.
    const auto types = scanner.getKnownPlugins().getTypes();

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
