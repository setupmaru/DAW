#include "PluginScanner.h"

namespace
{
// How long the destructor waits for the plugin being scanned to return before
// the thread is forcibly stopped.
constexpr int kStopTimeoutMs = 10000;

juce::AudioPluginFormat* findVst3Format (juce::AudioPluginFormatManager& formats)
{
    for (auto* format : formats.getFormats())
        if (format->getName() == "VST3")
            return format;

    return nullptr;
}
} // namespace

//==============================================================================
class PluginScanner::ScanThread : public juce::Thread
{
public:
    ScanThread (PluginScanner& o, juce::AudioPluginFormat& f, juce::FileSearchPath p,
                juce::WeakReference<PluginScanner> w, std::function<void()> done)
        : juce::Thread ("VST3 scan"), owner (o), format (f), paths (std::move (p)),
          weakOwner (std::move (w)), onFinished (std::move (done))
    {
    }

    void run() override
    {
        {
            // The last argument only matters for formats that need asynchronous
            // instantiation (AU); VST3 ignores it.
            juce::PluginDirectoryScanner scanner (owner.knownPlugins, format, paths, true,
                                                  owner.getPedalFile(), true);
            juce::String pluginBeingScanned;

            while (! threadShouldExit() && scanner.scanNextFile (true, pluginBeingScanned))
            {
            }
        }

        if (threadShouldExit())
            return;   // the owner is being destroyed

        owner.saveCache();

        // The owner may be destroyed before this message is delivered: the weak
        // reference (created on the message thread) is checked there.
        juce::MessageManager::callAsync ([weak = weakOwner, done = onFinished]() mutable
        {
            if (auto* self = weak.get())
                self->scanFinished (std::move (done));
        });
    }

private:
    PluginScanner& owner;
    juce::AudioPluginFormat& format;
    const juce::FileSearchPath paths;
    juce::WeakReference<PluginScanner> weakOwner;
    std::function<void()> onFinished;
};

//==============================================================================
PluginScanner::PluginScanner (juce::AudioPluginFormatManager& formats, juce::File cache)
    : formatManager (formats), cacheFile (std::move (cache))
{
}

PluginScanner::~PluginScanner()
{
    if (scanThread != nullptr)
        scanThread->stopThread (kStopTimeoutMs);
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
    if (isScanning())
        return;

    auto* format = findVst3Format (formatManager);

    if (format == nullptr)
    {
        juce::MessageManager::callAsync ([weak = juce::WeakReference<PluginScanner> (this),
                                          done = std::move (onFinished)]() mutable
        {
            if (weak.get() != nullptr && done)
                done();
        });
        return;
    }

    // A previous scan thread has finished its work (scanning is false) but may
    // still be returning from run(): join it before deleting it.
    if (scanThread != nullptr)
        scanThread->stopThread (-1);

    scanThread.reset();

    // Read on the message thread; the scan thread only gets a copy.
    auto paths = format->getDefaultLocationsToSearch();
    cacheFile.getParentDirectory().createDirectory();

    scanning = true;
    scanThread = std::make_unique<ScanThread> (*this, *format, paths,
                                               juce::WeakReference<PluginScanner> (this),
                                               std::move (onFinished));
    scanThread->startThread();
}

bool PluginScanner::isScanning() const
{
    return scanning.load();
}

juce::KnownPluginList& PluginScanner::getKnownPlugins()
{
    return knownPlugins;
}

void PluginScanner::saveCache()
{
    if (auto xml = knownPlugins.createXml())
    {
        cacheFile.getParentDirectory().createDirectory();
        xml->writeTo (cacheFile);
    }
}

void PluginScanner::scanFinished (std::function<void()> onFinished)
{
    scanning = false;

    if (onFinished)
        onFinished();
}
