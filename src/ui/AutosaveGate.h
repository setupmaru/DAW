#pragma once

// Decides when the chain may be written to last-chain.xml, so that file never
// loses a chain the user still has. Pure logic (no files, no JUCE), so every
// launch scenario can be unit-tested.
//
//   start ──launchRestoreStarted──▶ restoring ──finished(clean)──▶ open
//     │                                 │
//     │ (nothing to restore)            └──finished(problem)──▶ held ──userEdited──▶ open
//     └──────nothingToRestore──────────────────────────────────────────────────────▶ open
//
// - Before the launch restore has finished, nothing is saved: the app starts
//   with an empty chain, and saving it would erase the real one.
// - After a launch restore that left plugins out, saves stay held until the
//   user edits the chain by hand. Quitting without touching anything keeps the
//   full file, so the next launch restores everything once the plugin is back.
//   (Knob changes inside the plugins that did load are not saved meanwhile:
//   the price of never dropping a plugin from the file.)
// - Device changes, sample-rate changes and other model notifications are not
//   user edits and never release the hold; only explicit edit actions do.
class AutosaveGate
{
public:
    void nothingToRestore()                 { state = State::open; }
    void launchRestoreStarted()             { state = State::restoring; }
    void launchRestoreFinished (bool clean) { state = clean ? State::open : State::held; }

    // Bypass, move, remove, add, preset load: the user has taken over the chain.
    void userEdited()
    {
        if (state == State::held)
            state = State::open;
    }

    // chainIsRestoring: a preset load (or the launch restore) is in progress;
    // a half-built chain is never saved.
    bool savesAllowed (bool chainIsRestoring) const
    {
        return state == State::open && ! chainIsRestoring;
    }

    bool isHeld() const { return state == State::held; }

private:
    enum class State { start, restoring, held, open };
    State state = State::start;
};
