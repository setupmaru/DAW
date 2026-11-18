#include "PluginScanner.h"

namespace
{
// Pause between two plugin files, so queued UI messages run in between.
constexpr int kScanIntervalMs = 10;

juce::AudioPluginFormat* findVst3Format (juce::AudioPluginFormatManager& formats)
{
    for (auto* format : formats.getFormats())
        if (format->getName() == "VST3")
            return format;

    return nullptr;
}
} // namespace

//==============================================================================
PluginScanner::PluginScanner (juce::AudioPluginFormatManager& formats, juce::File cache)
    : formatManager (formats), cacheFile (std::move (cache))
{
}

PluginScanner::~PluginScanner()
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopTimer();
    directoryScanner.reset();   // before knownPlugins, which it refers to
}

juce::File PluginScanner::defaultCacheFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("DAW")
        .getChildFile ("plugins.xml");
}

juce::File PluginScanner::getPedalFile() const
{
    return cacheFile.getSiblingFile ("scan-pedal.txt");
}

void PluginScanner::loadCache()
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (! isScanning());

    knownPlugins.clear();

    if (cacheFile.existsAsFile())
        if (auto xml = juce::parseXML (cacheFile))
            knownPlugins.recreateFromXml (*xml);

    // A plugin named in the pedal file crashed the app during the last scan.
    juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (knownPlugins, getPedalFile());
}

void PluginScanner::startScan (std::function<void()> onFinished)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (onFinished)
        pendingCallbacks.push_back (std::move (onFinished));

    if (scanning)
        return;   // the callback runs when the current scan finishes

    scanning = true;
    cacheFile.getParentDirectory().createDirectory();

    // Without a VST3 format there is nothing to scan; the first tick finishes.
    if (auto* format = findVst3Format (formatManager))
        directoryScanner = std::make_unique<juce::PluginDirectoryScanner> (
            knownPlugins, *format, format->getDefaultLocationsToSearch(), true, getPedalFile());

    startTimer (kScanIntervalMs);
}

bool PluginScanner::isScanning() const
{
    return scanning;
}

juce::KnownPluginList& PluginScanner::getKnownPlugins()
{
    return knownPlugins;
}

void PluginScanner::timerCallback()
{
    juce::String pluginBeingScanned;

    // One file per tick. scanNextFile returns false once no file is left.
    if (directoryScanner == nullptr || ! directoryScanner->scanNextFile (true, pluginBeingScanned))
        finishScan();
}

void PluginScanner::finishScan()
{
    stopTimer();
    directoryScanner.reset();
    saveCache();
    scanning = false;

    // Take the callbacks out first: one may start a new scan (queueing its own
    // callback) or destroy this object, so members are not touched afterwards.
    auto callbacks = std::move (pendingCallbacks);
    pendingCallbacks.clear();

    for (auto& callback : callbacks)
        callback();
}

void PluginScanner::saveCache()
{
    if (auto xml = knownPlugins.createXml())
    {
        cacheFile.getParentDirectory().createDirectory();
        xml->writeTo (cacheFile);
    }
}
