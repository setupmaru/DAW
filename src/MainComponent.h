#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "audio/AudioEngine.h"
#include "plugins/PluginChain.h"
#include "plugins/PluginEditorWindow.h"
#include "plugins/PluginScanner.h"
#include "ui/AutosaveGate.h"
#include "ui/ChainListComponent.h"
#include "ui/LevelMeter.h"
#include "ui/PresetStore.h"

#include <map>
#include <memory>

// The single main screen (design.md 5.3):
//
//   [notice strip: space always reserved, shown only with a message]
//   [오디오 설정] [프리셋 ▼]   status line   입력 meter
//   chain rows ... [+ 플러그인 추가]
//   [플러그인 다시 스캔] [출력 켜짐|음소거 중] [모노]  출력 meter
//
// Persistence (docs/frontend-backend-interface.md §3): the mute state is
// remembered in settings.xml; the chain is saved to last-chain.xml (2 s after a
// change, every minute, and at exit) and restored at launch; named presets
// live in presets/.
//
// Owns the audio engine, the VST3 format manager, the scanner, the chain model
// and the plugin editor windows, and connects them to the controls.
//
// Threads: message thread only.
class MainComponent : public juce::Component,
                      private juce::ChangeListener,
                      private juce::MultiTimer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum TimerId { noticeTimer = 1, statusTimer = 2, autosaveTimer = 3, periodicSaveTimer = 4, restorePedalTimer = 5 };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback (int timerId) override;   // timed notice / status line

    void syncWithDevice();
    void showNoDeviceNotice();
    void updateStatus();
    void showNotice (const juce::String& text, bool sticky = false, int durationMs = 0);
    void clearNotice();          // the device problem is solved: removes everything
    void endTimedNotice();       // back to the device problem message, or nothing

    void chainChanged();
    void showAudioSettings();
    void showAddPluginMenu();
    void startScan();
    void updateScanButton();
    void openEditor (int id);
    void closeEditor (int id);

    // Output mute
    void setMuted (bool muted);
    void updateMuteButton();

    // Mono output (left = right), remembered like the mute state
    void setMono (bool mono);
    void updateMonoButton();

    // Presets and the automatically saved chain
    void showPresetMenu();
    void askPresetNameAndSave();
    void savePreset (const juce::String& name);
    void confirmAndLoadPreset (const juce::String& name);
    void confirmAndDeletePreset (const juce::String& name);
    enum class RestoreKind { launch, preset };
    void restoreChain (const juce::XmlElement& state, const juce::String& what, RestoreKind kind);
    void restoreLastChain();
    void scheduleAutosave();
    void saveLastChainNow (bool reportErrors = true);
    void updateEditingEnabled();

    // --- Owned objects. Declaration order matters: members are destroyed in
    // reverse order, which gives the required teardown order:
    //   1. editor windows (an editor must not outlive its processor),
    //   2. the PluginChain (deletes retired slots; needs the ChainProcessor),
    //   3. the PluginScanner (needs the format manager),
    //   4. the AudioEngine (its destructor shuts the device down; it owns the
    //      ChainProcessor, which deletes the slots still in the chain),
    //   5. the AudioPluginFormatManager (plugin instances, created through it,
    //      are all gone by now).
    // The UI controls come after the engine, so the level meters (which read
    // the engine's meter sources from a timer) die before it.
    PresetStore store;   // files only; no plugin or audio objects inside
    juce::AudioPluginFormatManager formatManager;
    AudioEngine engine;
    PluginScanner scanner;
    PluginChain chain;

    juce::TooltipWindow tooltipWindow { nullptr, 700 };   // on the desktop: not clipped to this window
    juce::Label noticeLabel, statusLabel;
    juce::TextButton audioSettingsButton, presetButton, scanButton, muteButton, monoButton;
    LevelMeter inputMeter, outputMeter;
    ChainListComponent chainList;

    std::map<int, std::unique_ptr<PluginEditorWindow>> editorWindows;   // entry id -> window

    // The audio settings dialog is owned by the desktop once launched; it holds
    // a reference to the device manager, so the destructor deletes it if open.
    juce::Component::SafePointer<juce::DialogWindow> settingsDialog;

    // The "save preset" name dialog. Owned here (not self-deleting) so its text
    // can be read in the result callback; destroyed with this component.
    std::unique_ptr<juce::AlertWindow> presetNameDialog;

    // When last-chain.xml may be written (see AutosaveGate).
    AutosaveGate autosaveGate;
    bool autosaveErrorShown = false;   // a failing autosave is reported once

    // The device problem message, if any. It stays until the problem is solved;
    // a timed notice may cover it for a while and it comes back afterwards.
    juce::String stickyNoticeText;

    // Device tracking for beginner-facing notices.
    juce::String startupDeviceError;   // why the device failed to open at launch, kept until one opens
    juce::String lastDeviceName;       // the device last seen open; empty before the first one

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
