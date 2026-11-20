#include <juce_core/juce_core.h>

#include "ui/AutosaveGate.h"

// The rules that keep last-chain.xml from losing a chain (MainComponent's
// autosave, timers and exit save all ask the gate first).
class AutosaveGateTests : public juce::UnitTest
{
public:
    AutosaveGateTests() : juce::UnitTest ("AutosaveGateTests") {}

    void runTest() override
    {
        beginTest ("nothing is saved before the launch restore has finished");
        {
            AutosaveGate gate;
            expect (! gate.savesAllowed (false));   // the app starts empty

            gate.launchRestoreStarted();
            expect (! gate.savesAllowed (false));
            expect (! gate.savesAllowed (true));
        }

        beginTest ("a clean launch restore opens saving, but never mid-restore");
        {
            AutosaveGate gate;
            gate.launchRestoreStarted();
            gate.launchRestoreFinished (true);
            expect (gate.savesAllowed (false));
            expect (! gate.savesAllowed (true));   // e.g. a preset load in progress
        }

        beginTest ("no file to restore: saving is open at once");
        {
            AutosaveGate gate;
            gate.nothingToRestore();
            expect (gate.savesAllowed (false));
        }

        beginTest ("a partly failed launch restore holds saves until the user edits");
        {
            AutosaveGate gate;
            gate.launchRestoreStarted();
            gate.launchRestoreFinished (false);
            expect (gate.isHeld());
            expect (! gate.savesAllowed (false));   // 2 s timer, 60 s timer and exit save alike

            gate.userEdited();
            expect (! gate.isHeld());
            expect (gate.savesAllowed (false));
        }

        beginTest ("user edits before or during the launch restore do not open saving early");
        {
            AutosaveGate gate;
            gate.userEdited();
            expect (! gate.savesAllowed (false));

            gate.launchRestoreStarted();
            gate.userEdited();
            expect (! gate.savesAllowed (false));

            gate.launchRestoreFinished (false);
            expect (gate.isHeld());
        }
    }
};

static AutosaveGateTests autosaveGateTests;
