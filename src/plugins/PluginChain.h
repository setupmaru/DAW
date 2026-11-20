#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "audio/ChainProcessor.h"

#include <functional>
#include <memory>
#include <vector>

// One effect as the UI sees it. `processor` is a non-owning pointer into a slot
// the ChainProcessor owns. It is valid only while the entry is in the model:
// the model always drops an entry before its slot can be retired and deleted.
struct ChainEntry
{
    int id;
    juce::String name;
    bool bypassed;
    juce::AudioProcessor* processor;
    juce::PluginDescription description;
};

// The message-thread model of the effect chain. Every edit is posted to the
// ChainProcessor first; the model only changes when the post succeeds, so the
// model and the audio side never disagree about order, membership or bypass.
//
// Threads: message thread only (constructor, destructor and every method).
//
// Lifetime: the ChainProcessor must outlive this object (declare it first).
// Destroying the PluginChain stops its timer and deletes the slots waiting in
// the retired queue; slots still in the chain stay with the ChainProcessor,
// whose destructor deletes them. Plugin creations still pending in addPlugin()
// when this object dies are discarded when they complete (the new instance is
// deleted on the message thread, nothing is posted).
class PluginChain : private juce::Timer
{
public:
    using CreateCallback = std::function<void (std::unique_ptr<juce::AudioProcessor>, const juce::String&)>;
    // Optional factory for non-hosted processors/tests. Completion must run on
    // the message thread exactly once; synchronous completion is also allowed.
    using ProcessorFactory = std::function<void (const juce::PluginDescription&, double, int, CreateCallback)>;
    PluginChain (ChainProcessor&, juce::AudioPluginFormatManager&, ProcessorFactory = {});
    ~PluginChain() override;

    // Fallback spec before the engine has ever been prepared. Afterwards new
    // slots always read the engine's current spec, without waiting for the UI.
    void setProcessingSpec (double sampleRate, int maxBlockSize);

    // Sets a stereo in/out layout, prepares a slot and posts it to the end of
    // the chain. Returns false (and calls onError, except where noted) when the
    // processor has no stereo layout, the model already holds kMaxSlots entries
    // or the command queue is full. On false the processor is deleted and the
    // model is unchanged.
    bool addProcessor (std::unique_ptr<juce::AudioProcessor>, const juce::String& name);

    // Creates the plugin asynchronously, then calls addProcessor. Failures are
    // reported through onError.
    void addPlugin (const juce::PluginDescription&);

    // Each returns false when the id is unknown or the command queue is full;
    // the model is then unchanged. remove() drops the entry at once.
    bool remove (int id);
    bool move (int id, int newIndex);   // newIndex is clamped into the chain
    bool setBypassed (int id, bool bypassed);

    const std::vector<ChainEntry>& getEntries() const;

    struct RestoreResult
    {
        int restored = 0;
        juce::StringArray failedNames;
    };
    std::unique_ptr<juce::XmlElement> createStateXml() const;
    void restoreFromXml (const juce::XmlElement&, std::function<void (RestoreResult)> onDone);
    bool isRestoring() const;

    std::function<void()> onChanged;                       // the entries changed
    std::function<void (const juce::String&)> onError;    // Korean, user-facing

    // Deletes every slot in the ChainProcessor's retired queue. Called by a
    // 30 Hz timer; tests call it directly.
    void pumpRetired();

private:
    void timerCallback() override;
    int indexOf (int id) const;
    void changed();
    void error (const juce::String& message);
    ChainProcessor::ProcessingSpec processingSpec() const;
    bool insertProcessor (std::unique_ptr<juce::AudioProcessor>, const juce::String&,
                          const juce::PluginDescription&);
    void createProcessor (const juce::PluginDescription&, CreateCallback);
    void advanceRestore();
    void scheduleRestore();
    struct RestoreState;
    std::unique_ptr<RestoreState> restore;
    ProcessorFactory processorFactory;
    unsigned int generation = 0;

    ChainProcessor& chain;
    juce::AudioPluginFormatManager& formatManager;
    std::vector<ChainEntry> entries;
    int nextId = 1;
    double sampleRate = 48000.0;
    int blockSize = 512;

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginChain)
    JUCE_DECLARE_NON_COPYABLE (PluginChain)
};
