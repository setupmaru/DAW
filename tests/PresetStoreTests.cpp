#include <juce_core/juce_core.h>

#include "ui/PresetStore.h"

// File handling behind the preset menu and the automatically saved chain.
// Works in a temporary folder; no audio, plugins or display needed.
class PresetStoreTests : public juce::UnitTest
{
public:
    PresetStoreTests() : juce::UnitTest ("PresetStoreTests") {}

    void runTest() override
    {
        beginTest ("empty folder: no presets, no last chain");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (store.getPresetNames().isEmpty());
            expect (! store.hasLastChain());
            expect (store.loadLastChain() == nullptr);
            expect (! store.backupLastChain());
        }

        beginTest ("save, list sorted, load and delete presets");
        {
            TempRoot root;
            PresetStore store (root.dir);

            expect (store.savePreset ("Talk", chainXml (2)));
            expect (store.savePreset ("game 2", chainXml (1)));
            expect (store.savePreset ("Game 10", chainXml (3)));

            expectEquals (store.getPresetNames().joinIntoString ("|"), juce::String ("game 2|Game 10|Talk"));
            expect (store.presetExists ("Talk"));

            const auto loaded = store.loadPreset ("Game 10");
            expect (loaded != nullptr);

            if (loaded != nullptr)
            {
                expect (loaded->hasTagName ("DAWChain"));
                expectEquals (loaded->getNumChildElements(), 3);
            }

            expect (store.deletePreset ("Talk"));
            expect (! store.presetExists ("Talk"));
            expectEquals (store.getPresetNames().size(), 2);
            expect (! store.deletePreset ("Talk"));   // already gone
        }

        beginTest ("saving a preset with an existing name overwrites it");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (store.savePreset ("Talk", chainXml (1)));
            expect (store.savePreset ("Talk", chainXml (4)));
            expectEquals (store.getPresetNames().size(), 1);

            const auto loaded = store.loadPreset ("Talk");
            expect (loaded != nullptr && loaded->getNumChildElements() == 4);
        }

        beginTest ("typed names become legal file names; unusable names are refused");
        {
            TempRoot root;
            PresetStore store (root.dir);

            expectEquals (store.toStoredName ("  a/b:c  "), juce::String ("abc"));
            expectEquals (store.toStoredName ("..hidden.."), juce::String ("hidden"));
            expectEquals (store.toStoredName (juce::String (juce::CharPointer_UTF8 ("방송 프리셋"))),
                          juce::String (juce::CharPointer_UTF8 ("방송 프리셋")));
            expectEquals (store.toStoredName ("tab\there\nnow"), juce::String ("tabherenow"));
            expectEquals (store.toStoredName ("con"), juce::String ("_con"));
            expectEquals (store.toStoredName ("COM3"), juce::String ("_COM3"));
            expectEquals (store.toStoredName ("COM0"), juce::String ("COM0"));   // not reserved
            expectEquals (store.toStoredName ("nul.txt"), juce::String ("_nul.txt"));
            expectEquals (store.toStoredName ("Live_temp2"), juce::String ("Live-temp2"));
            expectEquals (store.toStoredName ("Live_tempo"), juce::String ("Live_tempo"));   // not hex: kept

            expect (store.savePreset (store.toStoredName ("Live_temp2"), chainXml (1)));
            expect (store.getPresetNames().contains ("Live-temp2"));
            expect (store.toStoredName ("/:*?").isEmpty());
            expect (store.toStoredName ("   ").isEmpty());

            const auto longName = store.toStoredName (juce::String::repeatedString (juce::CharPointer_UTF8 ("가"), 200));
            expectEquals (longName.length(), PresetStore::kMaxNameLength);
            expect (store.savePreset (longName, chainXml (1)));
            expect (store.presetExists (longName));
        }

        beginTest ("stored names are used as they are and cannot leave the presets folder");
        {
            TempRoot root;
            PresetStore store (root.dir);

            for (auto* bad : { "", ".", "..", "../outside", "a/b", "a\\b" })
            {
                expect (! store.savePreset (bad, chainXml (1)), bad);
                expect (! store.presetExists (bad), bad);
                expect (store.loadPreset (bad) == nullptr, bad);
                expect (! store.deletePreset (bad), bad);
            }

            expect (! root.dir.getChildFile ("outside.xml").exists());
            expect (store.getPresetNames().isEmpty());
        }

        beginTest ("a hand-copied preset with characters a typed name would lose still loads and deletes");
        {
            TempRoot root;
            PresetStore store (root.dir);
            const auto presets = root.dir.getChildFile ("presets");
            expect (presets.createDirectory().wasOk());
            expect (chainXml (2).writeTo (presets.getChildFile ("Talk #2.xml")));

            expectEquals (store.getPresetNames().joinIntoString ("|"), juce::String ("Talk #2"));
            const auto loaded = store.loadPreset ("Talk #2");
            expect (loaded != nullptr && loaded->getNumChildElements() == 2);
            expect (store.deletePreset ("Talk #2"));
            expect (store.getPresetNames().isEmpty());
        }

        beginTest ("temporary files left by an interrupted save are not listed");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (store.savePreset ("Mic", chainXml (1)));
            expect (chainXml (1).writeTo (root.dir.getChildFile ("presets").getChildFile ("Mic_temp1a2b3c4d.xml")));
            expect (chainXml (1).writeTo (root.dir.getChildFile ("presets").getChildFile ("my_tempo.xml")));

            expectEquals (store.getPresetNames().joinIntoString ("|"), juce::String ("Mic|my_tempo"));
        }

        beginTest ("unreadable preset and last-chain files load as nullptr");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (store.savePreset ("Bad", chainXml (1)));
            // (JUCE's parser is lenient about a missing '>', so use something
            // that is not XML at all. Validating the chain format is the
            // backend's job in restoreFromXml.)
            expect (root.dir.getChildFile ("presets").getChildFile ("Bad.xml").replaceWithText ("not xml"));
            expect (store.loadPreset ("Bad") == nullptr);

            expect (root.dir.getChildFile ("last-chain.xml").replaceWithText ("not xml"));
            expect (store.hasLastChain());
            expect (store.loadLastChain() == nullptr);
        }

        beginTest ("last chain: save, load, back up");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (store.saveLastChain (chainXml (2)));
            expect (store.hasLastChain());

            const auto loaded = store.loadLastChain();
            expect (loaded != nullptr && loaded->getNumChildElements() == 2);

            expect (store.backupLastChain());
            expect (store.saveLastChain (chainXml (0)));

            const auto backup = juce::parseXML (store.getLastChainBackupFile());
            expect (backup != nullptr && backup->getNumChildElements() == 2);
            const auto now = store.loadLastChain();
            expect (now != nullptr && now->getNumChildElements() == 0);
        }

        beginTest ("a last chain can be set aside, replacing an older one");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (! store.setLastChainAside());   // nothing to move

            expect (store.saveLastChain (chainXml (1)));
            expect (store.setLastChainAside());
            expect (! store.hasLastChain());

            expect (store.saveLastChain (chainXml (3)));
            expect (store.setLastChainAside());
            const auto broken = juce::parseXML (store.getLastChainBrokenFile());
            expect (broken != nullptr && broken->getNumChildElements() == 3);
        }

        beginTest ("restore pedal: down while restoring, up afterwards");
        {
            TempRoot root;
            PresetStore store (root.dir);
            expect (! store.restorePedalIsDown());
            store.pressRestorePedal();
            expect (store.restorePedalIsDown());
            expect (PresetStore (root.dir).restorePedalIsDown());   // what the next launch sees
            store.releaseRestorePedal();
            expect (! store.restorePedalIsDown());
        }

        beginTest ("settings persist across instances, in a folder that did not exist");
        {
            TempRoot root;
            const auto fresh = root.dir.getChildFile ("not-yet");
            {
                PresetStore store (fresh);
                store.getSettings().setValue ("outputMuted", true);
                expect (store.saveSettings());
            }
            PresetStore again (fresh);
            expect (again.getSettings().getBoolValue ("outputMuted", false));
        }
    }

private:
    struct TempRoot
    {
        TempRoot()
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getNonexistentChildFile ("DAWPresetStoreTest", {}))
        {
            dir.createDirectory();
        }

        ~TempRoot() { dir.deleteRecursively(); }

        juce::File dir;
    };

    static juce::XmlElement chainXml (int plugins)
    {
        juce::XmlElement xml ("DAWChain");
        xml.setAttribute ("version", 1);

        for (int i = 0; i < plugins; ++i)
            xml.createNewChildElement ("Plugin")->setAttribute ("bypassed", 0);

        return xml;
    }
};

static PresetStoreTests presetStoreTests;
