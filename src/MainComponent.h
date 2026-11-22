#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/MixEngine.h"
#include "plugins/PluginEditorWindow.h"
#include "plugins/PluginScanner.h"
#include "session/Bouncer.h"
#include "session/Session.h"
#include "ui/AppActions.h"
#include "ui/EditState.h"
#include "ui/EditWindow.h"
#include "ui/MixWindow.h"
#include "ui/TransportBar.h"

#include <map>
#include <memory>

// The main window's content: menu bar, transport bar, the edit window or the
// mix window, and a status line.
//
//   ┌ 파일 편집 트랙 보기 설정 ──────────────────────────────────────────┐
//   │ [슬립|그리드] [트림|선택|잡기] ⏮ ■ ▶ ● ⟲  1|1|000   BPM  [편집|믹스] │
//   ├───────────────┬─────────────────────────────────────────────────────┤
//   │ track headers │ ruler / clips / waveforms / playhead                │
//   ├───────────────┴─────────────────────────────────────────────────────┤
//   │ session name · messages                       device · rate · CPU  │
//   └─────────────────────────────────────────────────────────────────────┘
//
// Owns the engine, the plugin scanner, the session and every window, and
// implements the application commands (menus and shortcuts).
//
// Threads: message thread only.
class MainComponent : public juce::Component,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      public AppActions,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Asks to save unsaved changes, then calls `quit` (unless cancelled).
    void requestQuit (std::function<void()> quit);
    // Command line: a session file to open or audio files to import, plus
    // "--screenshot <png> [--mix]" (saves a picture of the window and quits).
    void handleCommandLine (const juce::StringArray& args);

    juce::ApplicationCommandManager& getCommandManager() noexcept { return commandManager; }

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override;
    void menuItemSelected (int, int) override {}

    // AppActions
    void showInsertMenu (int trackId, int entryId, juce::Component& anchor) override;
    void openPluginEditor (int trackId, int entryId) override;
    void importFiles (const juce::StringArray& files, int trackId, juce::int64 position) override;
    void locate (juce::int64 position) override;
    void showTrackMenu (int trackId) override;
    void showMessage (const juce::String&) override;

private:
    class BusyOverlay;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    PluginChain* chainFor (int trackId);
    void closeStaleEditors();
    void closeEditorsOf (int trackId);

    void newSession();
    void openSessionChooser();
    void openSession (const juce::File&);
    void saveSession (std::function<void (bool saved)> then = {});
    void saveSessionAs (std::function<void (bool saved)> then = {});
    void confirmDiscard (std::function<void()> proceed);
    void importChooser();
    void bounceChooser();
    void showAudioSettings();
    void renameTrackDialog (int trackId);
    void deleteTrack (int trackId);

    void togglePlay();
    void toggleRecord();
    void syncLoop();
    void zoomBy (double factor);
    void zoomToFit();
    void showMix (bool);
    void updateTitle();
    void deviceChanged();
    juce::File defaultSessionFolder() const;
    std::vector<int> editTracks() const;   // the tracks an edit command applies to

    // --- Owned objects. Declaration order is teardown order reversed:
    // windows and editors die first, then the session (its PluginChains and
    // track references), then the scanner, then the engine (which deletes the
    // last snapshots and with them the plugin instances), then the formats.
    std::unique_ptr<juce::PropertiesFile> settings;
    juce::AudioPluginFormatManager pluginFormats;
    juce::AudioFormatManager audioFormats;
    MixEngine engine;
    PluginScanner scanner;
    std::unique_ptr<Session> session;

    EditState editState;
    juce::ApplicationCommandManager commandManager;
    juce::TooltipWindow tooltipWindow { nullptr, 700 };
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<TransportBar> transport;
    std::unique_ptr<EditWindow> editWindow;
    std::unique_ptr<MixWindow> mixWindow;
    bool mixShown = false;

    juce::String message, persistentWarning, startupError;
    juce::uint32 messageUntil = 0;
    juce::Rectangle<int> statusArea;

    std::map<std::pair<int, int>, std::unique_ptr<PluginEditorWindow>> editorWindows;   // (track, entry) -> window
    juce::Component::SafePointer<juce::DialogWindow> settingsDialog;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> textDialog;
    std::unique_ptr<Bouncer> bouncer;
    std::unique_ptr<BusyOverlay> overlay;
    juce::String lastWindowTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
