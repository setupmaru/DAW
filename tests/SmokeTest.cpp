#include <juce_core/juce_core.h>

// Placeholder that verifies the test runner wiring.
struct SmokeTest : public juce::UnitTest
{
    SmokeTest() : juce::UnitTest ("SmokeTest", "DAW") {}

    void runTest() override
    {
        beginTest ("runner is wired");
        expect (true);
    }
};

static SmokeTest smokeTest;
