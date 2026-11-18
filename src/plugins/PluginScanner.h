#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <vector>

// Finds VST3 plugins in the default VST3 locations and keeps the result in an
// XML cache, so the app does not rescan on every start.
//
// Threads: everything, the scan included, runs on the message thread. A scan
// is incremental: a timer scans one plugin file per tick (as JUCE's
// PluginListComponent does by default), so the UI keeps responding between
// files. Scanning must not run on another thread: JUCE's VST3 hosting keeps
// module handles in an unlocked static set that plugin creation (PluginChain::
// addPlugin) and deletion (PluginChain::pumpRetired) also touch on the message
// thread. A plugin that takes long to load blocks the message thread for that
// one file; the audio thread is not affected.
//
// Crash safety: plugins are scanned with a dead-man's-pedal file
// (scan-pedal.txt next to the cache). A plugin that crashes the app during a
// scan is blacklisted the next time loadCache() runs.
//
// Lifetime: the format manager must outlive this object. The destructor stops
// the timer; callbacks of an unfinished scan are dropped, never called.
class PluginScanner : private juce::Timer
{
public:
    PluginScanner (juce::AudioPluginFormatManager&, juce::File cacheFile);
    ~PluginScanner() override;

    // Reads the cache (an empty list if it is missing or unreadable) and
    // blacklists a plugin that crashed a previous scan. Not while scanning.
    void loadCache();

    // Starts scanning the default VST3 locations, unless a scan is already
    // running. Either way onFinished is queued and called (asynchronously, on
    // the message thread, once each) when the running scan has finished and
    // the cache has been saved.
    void startScan (std::function<void()> onFinished);
    bool isScanning() const;

    juce::KnownPluginList& getKnownPlugins();

    // userApplicationDataDirectory/DAW/plugins.xml
    static juce::File defaultCacheFile();

private:
    void timerCallback() override;
    void finishScan();
    juce::File getPedalFile() const;
    void saveCache();

    juce::AudioPluginFormatManager& formatManager;
    const juce::File cacheFile;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::PluginDirectoryScanner> directoryScanner;   // null: nothing (left) to scan
    std::vector<std::function<void()>> pendingCallbacks;
    bool scanning = false;

    JUCE_DECLARE_NON_COPYABLE (PluginScanner)
};
