#include "TestProcessors.h"

#include "plugins/PluginScanner.h"

// The scanner's control flow (timer-driven scan, callback queueing, cache
// write, destruction mid-scan) with an empty format manager, so no VST3 plugin
// is needed. Scanning real VST3 files is not covered here.
struct PluginScannerTests : public juce::UnitTest
{
    PluginScannerTests() : juce::UnitTest ("PluginScannerTests", "DAW") {}

    void runTest() override
    {
        // expect() needs a current test: JUCE crashes on an expect before the
        // first beginTest when this suite runs first.
        beginTest ("a temporary cache directory can be created");

        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getNonexistentChildFile ("DAWScannerTest", "");
        expect (dir.createDirectory().wasOk());
        const auto cache = dir.getChildFile ("plugins.xml");

        testCallbacksQueuedWhileScanning (cache);
        testDestroyedMidScanDropsCallbacks (cache);

        dir.deleteRecursively();
    }

    void testCallbacksQueuedWhileScanning (const juce::File& cache)
    {
        beginTest ("startScan during a scan queues its callback");

        juce::AudioPluginFormatManager formats;
        PluginScanner scanner (formats, cache);
        scanner.loadCache();
        expectEquals (scanner.getKnownPlugins().getNumTypes(), 0);

        int first = 0, second = 0, third = 0;
        scanner.startScan ([&] { ++first; });
        expect (scanner.isScanning());
        scanner.startScan ([&] { ++second; });
        expectEquals (first + second, 0, "callbacks run asynchronously");

        expect (pumpMessagesUntil ([&] { return ! scanner.isScanning(); }), "scan finishes");
        expectEquals (first, 1);
        expectEquals (second, 1, "the queued callback runs when the scan finishes");
        expect (cache.existsAsFile(), "the cache is saved");

        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        expectEquals (first, 1, "each callback runs once");
        expectEquals (second, 1, "each callback runs once");

        scanner.startScan ([&] { ++third; });
        expect (pumpMessagesUntil ([&] { return third == 1; }), "a new scan can start afterwards");
        expectEquals (first + second, 2);

        PluginScanner reloaded (formats, cache);
        reloaded.loadCache();
        expectEquals (reloaded.getKnownPlugins().getNumTypes(), 0);
    }

    void testDestroyedMidScanDropsCallbacks (const juce::File& cache)
    {
        beginTest ("destroying the scanner mid-scan drops its callbacks");

        juce::AudioPluginFormatManager formats;
        int called = 0;

        {
            PluginScanner scanner (formats, cache);
            scanner.startScan ([&] { ++called; });
            expect (scanner.isScanning());
        }

        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        expectEquals (called, 0);
    }
};

static PluginScannerTests pluginScannerTests;
