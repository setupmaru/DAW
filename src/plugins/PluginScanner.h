#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>

// Finds VST3 plugins in the default VST3 locations and keeps the result in an
// XML cache, so the app does not rescan on every start.
//
// Scanning runs on a background thread. Plugins are scanned with a
// dead-man's-pedal file (scan-pedal.txt next to the cache): if a plugin crashes
// the app during a scan, it is blacklisted the next time loadCache() runs.
//
// Threads: every public method on the message thread. The KnownPluginList is
// filled from the scan thread while a scan runs (KnownPluginList locks
// internally); it sends its change messages asynchronously.
//
// Lifetime: the format manager must outlive this object. The destructor stops a
// running scan and waits for the plugin being scanned to finish; onFinished is
// not called after destruction.
class PluginScanner
{
public:
    PluginScanner (juce::AudioPluginFormatManager&, juce::File cacheFile);
    ~PluginScanner();

    // Reads the cache (an empty list if it is missing or unreadable) and
    // blacklists a plugin that crashed a previous scan.
    void loadCache();

    // Scans the default VST3 locations on a background thread, saves the cache,
    // then calls onFinished on the message thread. Ignored while a scan runs.
    void startScan (std::function<void()> onFinished);
    bool isScanning() const;

    juce::KnownPluginList& getKnownPlugins();

    // userApplicationDataDirectory/DAW/plugins.xml
    static juce::File defaultCacheFile();

private:
    class ScanThread;

    juce::File getPedalFile() const;
    void saveCache();
    void scanFinished (std::function<void()> onFinished);

    juce::AudioPluginFormatManager& formatManager;
    const juce::File cacheFile;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<ScanThread> scanThread;
    std::atomic<bool> scanning { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginScanner)
    JUCE_DECLARE_NON_COPYABLE (PluginScanner)
};
