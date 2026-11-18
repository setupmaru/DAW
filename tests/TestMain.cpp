#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (auto* result = runner.getResult (i))
            failures += result->failures;

    // No results means no test ran at all, which must not pass silently.
    if (runner.getNumResults() == 0)
        return 1;

    return failures > 0 ? 1 : 0;
}
