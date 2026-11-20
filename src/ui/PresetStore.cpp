#include "PresetStore.h"

namespace
{
constexpr const char* kPresetExtension = ".xml";

juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = -1;   // never automatic: saveSettings() writes and reports failure
    return options;
}

// Windows refuses these as file names, whatever the extension.
bool isReservedWindowsName (const juce::String& name)
{
    // "nul.txt" is a device too: only the part before the first dot counts.
    const auto upper = name.upToFirstOccurrenceOf (".", false, false).trimEnd().toUpperCase();

    for (auto* reserved : { "CON", "PRN", "AUX", "NUL" })
        if (upper == reserved)
            return true;

    if (upper.length() == 4 && (upper.startsWith ("COM") || upper.startsWith ("LPT")))
        return juce::CharacterFunctions::isDigit (upper[3]) && upper[3] != '0';

    return false;
}
} // namespace

PresetStore::PresetStore (juce::File rootFolder)
    : root (std::move (rootFolder)),
      presetsDir (root.getChildFile ("presets")),
      lastChainFile (root.getChildFile ("last-chain.xml")),
      lastChainBackupFile (root.getChildFile ("last-chain.prev.xml")),
      lastChainBrokenFile (root.getChildFile ("last-chain.broken.xml")),
      restorePedalFile (root.getChildFile ("restore-pedal"))
{
    // Created up front: the settings file cannot be written into a missing
    // folder, and the first thing a new user does may be a mute toggle.
    root.createDirectory();
    settings = std::make_unique<juce::PropertiesFile> (root.getChildFile ("settings.xml"), settingsOptions());
}

juce::File PresetStore::defaultRoot()
{
    // The same folder PluginScanner::defaultCacheFile() uses.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("DAW");
}

bool PresetStore::saveSettings()
{
    root.createDirectory();
    return settings->saveIfNeeded();
}

//==============================================================================
bool PresetStore::isTemporaryFileName (const juce::String& name)
{
    // XmlElement::writeTo writes "<name>_temp<hex>.xml" first; a crash in
    // between can leave one behind. It is not a preset.
    const int pos = name.lastIndexOf ("_temp");
    return pos >= 0 && name.length() > pos + 5
           && name.substring (pos + 5).containsOnly ("0123456789abcdefABCDEF");
}

juce::StringArray PresetStore::getPresetNames() const
{
    juce::StringArray names;

    for (const auto& file : presetsDir.findChildFiles (juce::File::findFiles, false,
                                                       juce::String ("*") + kPresetExtension))
    {
        const auto name = file.getFileNameWithoutExtension();

        if (name.isNotEmpty() && ! isTemporaryFileName (name))
            names.add (name);
    }

    names.sortNatural();
    return names;
}

juce::String PresetStore::toStoredName (const juce::String& typed) const
{
    // Control characters (a pasted tab or newline) are not allowed in Windows
    // file names; createLegalFileName drops the other unusable characters.
    juce::String printable;

    for (auto c : typed)
        if (c >= 0x20)
            printable += juce::String::charToString (c);

    // Dots at the ends would confuse the extension (and ".." the folder).
    auto name = juce::File::createLegalFileName (printable.trim())
                    .trimCharactersAtStart (".")
                    .trimCharactersAtEnd (". ")
                    .trim()
                    .substring (0, kMaxNameLength)
                    .trim();

    if (isReservedWindowsName (name))
        name = "_" + name;

    // A typed name must never look like a leftover temporary file, or the
    // saved preset would be hidden from the menu ("Live_temp2" → "Live-temp2").
    if (isTemporaryFileName (name))
        name = name.replace ("_temp", "-temp");

    return name;
}

juce::File PresetStore::presetFile (const juce::String& storedName) const
{
    // Names come back from the menu exactly as stored, but a file can be put
    // in the folder by hand: anything that could leave presets/ is refused.
    if (storedName.isEmpty() || storedName == "." || storedName == ".."
        || storedName.containsAnyOf ("/\\") || storedName.containsChar (0))
        return {};

    const auto file = presetsDir.getChildFile (storedName + kPresetExtension);
    return file.getParentDirectory() == presetsDir ? file : juce::File();
}

bool PresetStore::presetExists (const juce::String& storedName) const
{
    const auto file = presetFile (storedName);
    return file != juce::File() && file.existsAsFile();
}

bool PresetStore::savePreset (const juce::String& storedName, const juce::XmlElement& chainState)
{
    const auto file = presetFile (storedName);

    if (file == juce::File() || ! presetsDir.createDirectory().wasOk())
        return false;

    return chainState.writeTo (file);   // temporary file, then swapped in
}

std::unique_ptr<juce::XmlElement> PresetStore::loadPreset (const juce::String& storedName) const
{
    const auto file = presetFile (storedName);
    return file == juce::File() ? nullptr : juce::parseXML (file);
}

bool PresetStore::deletePreset (const juce::String& storedName)
{
    // File::deleteFile() also returns true for a file that is not there.
    const auto file = presetFile (storedName);
    return file != juce::File() && file.existsAsFile() && file.deleteFile();
}

//==============================================================================
bool PresetStore::hasLastChain() const
{
    return lastChainFile.existsAsFile();
}

bool PresetStore::saveLastChain (const juce::XmlElement& chainState)
{
    if (! root.createDirectory().wasOk())
        return false;

    return chainState.writeTo (lastChainFile);
}

std::unique_ptr<juce::XmlElement> PresetStore::loadLastChain() const
{
    return juce::parseXML (lastChainFile);
}

bool PresetStore::backupLastChain()
{
    return lastChainFile.existsAsFile() && lastChainFile.copyFileTo (lastChainBackupFile);
}

bool PresetStore::setLastChainAside()
{
    if (! lastChainFile.existsAsFile())
        return false;

    lastChainBrokenFile.deleteFile();
    return lastChainFile.moveFileTo (lastChainBrokenFile);
}

//==============================================================================
bool PresetStore::restorePedalIsDown() const
{
    return restorePedalFile.existsAsFile();
}

void PresetStore::pressRestorePedal()
{
    root.createDirectory();
    restorePedalFile.replaceWithText ("restoring");
}

void PresetStore::releaseRestorePedal()
{
    restorePedalFile.deleteFile();
}
