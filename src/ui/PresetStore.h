#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <memory>

// The app's files in the user's application data folder (DAW/), next to the
// scanner's plugins.xml:
//
//   DAW/settings.xml            app settings (output muted, ...)
//   DAW/last-chain.xml          the chain as it was at the last save, restored at launch
//   DAW/last-chain.prev.xml     the last chain that restored completely (backup)
//   DAW/last-chain.broken.xml   a last chain that could not be read or restored, set aside
//   DAW/restore-pedal           exists only while the launch restore runs (crash detection)
//   DAW/presets/<name>.xml      named presets
//
// The chain XML itself comes from PluginChain::createStateXml(); this class
// only stores and finds it. Writes go through a temporary file (XmlElement::
// writeTo), so a crash while saving never leaves a half-written file.
//
// Threads: message thread only.
class PresetStore
{
public:
    // root: the DAW/ folder. Tests may pass a temporary folder.
    explicit PresetStore (juce::File root = defaultRoot());

    static juce::File defaultRoot();

    // --- Settings. setValue() does not write the file; call saveSettings().
    juce::PropertiesFile& getSettings() { return *settings; }
    bool saveSettings();

    // --- Named presets.
    // The menu shows and passes back the names exactly as stored on disk
    // (getPresetNames); only a newly typed name goes through toStoredName.
    juce::StringArray getPresetNames() const;                       // sorted; temporary files left by a crash are skipped
    juce::String toStoredName (const juce::String& typed) const;   // empty if nothing usable remains
    bool presetExists (const juce::String& storedName) const;
    bool savePreset (const juce::String& storedName, const juce::XmlElement& chainState);
    std::unique_ptr<juce::XmlElement> loadPreset (const juce::String& storedName) const;   // nullptr: missing or unreadable
    bool deletePreset (const juce::String& storedName);

    static constexpr int kMaxNameLength = 60;   // characters; well inside every file system's limit

    // --- The automatically saved chain.
    bool hasLastChain() const;
    bool saveLastChain (const juce::XmlElement& chainState);
    std::unique_ptr<juce::XmlElement> loadLastChain() const;   // nullptr: missing or unreadable

    // Copies last-chain.xml to last-chain.prev.xml. Called after a launch
    // restore that loaded everything, so the backup is always a complete chain.
    bool backupLastChain();
    juce::File getLastChainBackupFile() const { return lastChainBackupFile; }

    // Moves last-chain.xml aside to last-chain.broken.xml (replacing an older
    // one), so a file that cannot be restored is kept but not retried.
    bool setLastChainAside();
    juce::File getLastChainBrokenFile() const { return lastChainBrokenFile; }

    // The restore pedal: written before the launch restore, removed when it
    // finishes (or the app quits). Still there at the next launch means the
    // app died while restoring, most likely inside a plugin.
    bool restorePedalIsDown() const;
    void pressRestorePedal();
    void releaseRestorePedal();

private:
    juce::File presetFile (const juce::String& storedName) const;   // invalid File for an unsafe name
    static bool isTemporaryFileName (const juce::String& nameWithoutExtension);

    juce::File root, presetsDir, lastChainFile, lastChainBackupFile, lastChainBrokenFile, restorePedalFile;
    std::unique_ptr<juce::PropertiesFile> settings;

    JUCE_DECLARE_NON_COPYABLE (PresetStore)
};
