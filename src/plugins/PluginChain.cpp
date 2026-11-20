#include "PluginChain.h"

#include "audio/ChainSlot.h"

namespace
{
juce::String utf8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

constexpr int kTimerHz = 30;

// Puts the main input and output buses in stereo, keeping any other bus as it
// is. False when the processor lacks a main input or output, or refuses stereo.
bool setStereoLayout (juce::AudioProcessor& processor)
{
    auto layout = processor.getBusesLayout();

    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
        return false;

    layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

    return processor.setBusesLayout (layout);
}
} // namespace

struct PluginChain::RestoreState
{
    struct Item
    {
        juce::PluginDescription description;
        juce::MemoryBlock state;
        bool bypassed = false;
    };
    std::vector<Item> items;
    size_t next = 0;
    bool clearing = true, creating = false;
    std::unique_ptr<ChainSlot> pending;
    RestoreResult result;
    juce::StringArray overflowNames;
    std::function<void (RestoreResult)> onDone;
};

//==============================================================================
PluginChain::PluginChain (ChainProcessor& c, juce::AudioPluginFormatManager& f, ProcessorFactory factory)
    : processorFactory (std::move (factory)), chain (c), formatManager (f)
{
    startTimerHz (kTimerHz);
}

PluginChain::~PluginChain()
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopTimer();

    // Delete what the audio side has handed back. No callbacks here: the owner
    // is being torn down. Slots still in the chain (or in the command queue)
    // belong to the ChainProcessor, which deletes them in its own destructor.
    // The weak reference is cleared after this body, still on the message
    // thread, so a pending addPlugin() completion sees nullptr.
    while (auto* slot = chain.popRetired())
        delete slot;
}

void PluginChain::setProcessingSpec (double newSampleRate, int maxBlockSize)
{
    jassert (newSampleRate > 0.0 && maxBlockSize > 0);
    sampleRate = newSampleRate;
    blockSize = maxBlockSize;
}

ChainProcessor::ProcessingSpec PluginChain::processingSpec() const
{
    const auto spec = chain.getProcessingSpec();
    return spec.sampleRate > 0.0 && spec.maxBlockSize > 0
        ? spec : ChainProcessor::ProcessingSpec { sampleRate, blockSize };
}

bool PluginChain::addProcessor (std::unique_ptr<juce::AudioProcessor> processor, const juce::String& name)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (isRestoring())
        return false;

    juce::PluginDescription description;
    description.name = name;
    description.pluginFormatName = "Internal";
    description.fileOrIdentifier = name;
    return insertProcessor (std::move (processor), name, description);
}

bool PluginChain::insertProcessor (std::unique_ptr<juce::AudioProcessor> processor,
                                   const juce::String& name, const juce::PluginDescription& description)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (processor != nullptr);

    if (processor == nullptr)
        return false;

    if ((int) entries.size() >= ChainProcessor::kMaxSlots)
    {
        error (utf8 ("체인이 가득 찼습니다(최대 16개). 플러그인을 추가할 수 없습니다: ") + name);
        return false;
    }

    if (! setStereoLayout (*processor))
    {
        error (utf8 ("이 플러그인은 스테레오를 지원하지 않아 추가할 수 없습니다: ") + name);
        return false;
    }

    auto* rawProcessor = processor.get();

    auto slot = std::make_unique<ChainSlot>();
    slot->id = nextId;
    slot->processor = std::move (processor);
    const auto spec = processingSpec();
    slot->prepare (spec.sampleRate, spec.maxBlockSize);

    ChainCommand cmd { ChainCommandType::Insert };
    cmd.slotId = slot->id;
    cmd.index = (int) entries.size();   // append, in model numbering
    cmd.bypassed = false;
    cmd.slot = slot.get();

    if (! chain.post (cmd))
    {
        error (utf8 ("오디오 처리 대기열이 가득 찼습니다. 잠시 후 다시 시도하세요: ") + name);
        return false;   // `slot` (and the processor) is deleted here
    }

    slot.release();   // the ChainProcessor owns it now
    entries.push_back ({ nextId++, name, false, rawProcessor, description });
    changed();
    return true;
}

void PluginChain::addPlugin (const juce::PluginDescription& description)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (isRestoring())
        return;

    createProcessor (description,
        [weak = juce::WeakReference<PluginChain> (this), description, epoch = generation]
        (std::unique_ptr<juce::AudioProcessor> instance, const juce::String& message)
        {
            auto* self = weak.get();
            if (self == nullptr || self->generation != epoch || self->isRestoring())
                return;

            if (instance == nullptr)
            {
                self->error (utf8 ("플러그인을 불러오지 못했습니다: ") + description.name
                             + (message.isNotEmpty() ? " (" + message + ")" : juce::String()));
                return;
            }
            self->insertProcessor (std::move (instance), description.name, description);
        });
}

void PluginChain::createProcessor (const juce::PluginDescription& description, CreateCallback callback)
{
    const auto spec = processingSpec();
    if (processorFactory)
        processorFactory (description, spec.sampleRate, spec.maxBlockSize, std::move (callback));
    else
        formatManager.createPluginInstanceAsync (description, spec.sampleRate, spec.maxBlockSize,
            [callback = std::move (callback)] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                               const juce::String& message)
            { callback (std::move (instance), message); });
}

bool PluginChain::remove (int id)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (isRestoring())
        return false;

    const int index = indexOf (id);

    if (index < 0)
        return false;

    ChainCommand cmd { ChainCommandType::Remove };
    cmd.slotId = id;

    if (! chain.post (cmd))
    {
        error (utf8 ("오디오 처리 대기열이 가득 찼습니다. 잠시 후 다시 시도하세요."));
        return false;
    }

    // Drop the entry now: its processor pointer must not be used once the slot
    // can come back through popRetired().
    entries.erase (entries.begin() + index);
    changed();
    return true;
}

bool PluginChain::move (int id, int newIndex)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (isRestoring())
        return false;

    const int index = indexOf (id);

    if (index < 0)
        return false;

    newIndex = juce::jlimit (0, (int) entries.size() - 1, newIndex);

    if (newIndex == index)
        return true;

    ChainCommand cmd { ChainCommandType::Move };
    cmd.slotId = id;
    cmd.index = newIndex;

    if (! chain.post (cmd))
    {
        error (utf8 ("오디오 처리 대기열이 가득 찼습니다. 잠시 후 다시 시도하세요."));
        return false;
    }

    auto entry = entries[(size_t) index];
    entries.erase (entries.begin() + index);
    entries.insert (entries.begin() + newIndex, entry);
    changed();
    return true;
}

bool PluginChain::setBypassed (int id, bool bypassed)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (isRestoring())
        return false;

    const int index = indexOf (id);

    if (index < 0)
        return false;

    ChainCommand cmd { ChainCommandType::SetBypass };
    cmd.slotId = id;
    cmd.bypassed = bypassed;

    if (! chain.post (cmd))
    {
        error (utf8 ("오디오 처리 대기열이 가득 찼습니다. 잠시 후 다시 시도하세요."));
        return false;
    }

    entries[(size_t) index].bypassed = bypassed;
    changed();
    return true;
}

const std::vector<ChainEntry>& PluginChain::getEntries() const
{
    return entries;
}

bool PluginChain::isRestoring() const
{
    return restore != nullptr;
}

std::unique_ptr<juce::XmlElement> PluginChain::createStateXml() const
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto xml = std::make_unique<juce::XmlElement> ("DAWChain");
    xml->setAttribute ("version", 1);
    for (const auto& entry : entries)
    {
        auto* item = xml->createNewChildElement ("Plugin");
        item->setAttribute ("bypassed", entry.bypassed ? 1 : 0);
        item->addChildElement (entry.description.createXml().release());
        juce::MemoryBlock state;
        entry.processor->getStateInformation (state);
        item->createNewChildElement ("State")->addTextElement (
            juce::Base64::toBase64 (state.getData(), state.getSize()));
    }
    return xml;
}

void PluginChain::restoreFromXml (const juce::XmlElement& xml, std::function<void (RestoreResult)> onDone)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto reject = [&]
    {
        juce::MessageManager::callAsync (
            [weak = juce::WeakReference<PluginChain> (this), done = std::move (onDone)]
            {
                if (auto* self = weak.get())
                {
                    self->error (utf8 ("체인 상태를 불러올 수 없습니다. 파일 형식이나 복원 상태를 확인하세요."));
                    if (weak != nullptr && done)
                        done ({});
                }
            });
    };
    if (isRestoring() || ! xml.hasTagName ("DAWChain") || xml.getStringAttribute ("version") != "1")
    {
        reject();
        return;
    }

    auto job = std::make_unique<RestoreState>();
    // Validate the entire document before touching the current model, even
    // the entries beyond the limit. Keep the input independent of its caller.
    for (auto* element : xml.getChildIterator())
    {
        RestoreState::Item item;
        const auto* description = element->getChildByName ("PLUGIN");
        const auto* state = element->getChildByName ("State");
        const auto bypassed = element->getStringAttribute ("bypassed");
        juce::MemoryOutputStream decoded;
        if (! element->hasTagName ("Plugin") || element->getNumChildElements() != 2
            || description == nullptr || ! item.description.loadFromXml (*description)
            || item.description.name.isEmpty() || item.description.pluginFormatName.isEmpty()
            || item.description.fileOrIdentifier.isEmpty()
            || state == nullptr || (bypassed != "0" && bypassed != "1")
            || ! juce::Base64::convertFromBase64 (decoded, state->getAllSubText())
            || juce::Base64::toBase64 (decoded.getData(), decoded.getDataSize()) != state->getAllSubText())
        {
            reject();
            return;
        }
        item.state = decoded.getMemoryBlock();
        item.bypassed = bypassed == "1";
        if (job->items.size() < (size_t) ChainProcessor::kMaxSlots)
            job->items.push_back (std::move (item));
        else
            job->overflowNames.add (item.description.name);
    }
    job->onDone = std::move (onDone);
    ++generation; // A normal Add started before this restore must not reappear.
    restore = std::move (job);
    scheduleRestore();
}

void PluginChain::scheduleRestore()
{
    juce::MessageManager::callAsync ([weak = juce::WeakReference<PluginChain> (this)]
    {
        if (auto* self = weak.get())
            self->advanceRestore();
    });
}

void PluginChain::advanceRestore()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! restore || restore->creating)
        return;

    if (restore->clearing)
    {
        if (! entries.empty())
        {
            // Queue pressure is temporary: the timer retries, and the model
            // only drops the processor after Remove has been accepted.
            if (! chain.post ({ ChainCommandType::Remove, entries.front().id }))
                return;
            entries.erase (entries.begin());
            scheduleRestore();
            changed(); // Close editors before retirement can delete anything.
            return;
        }
        restore->clearing = false;
    }

    if (restore->pending)
    {
        auto& slot = restore->pending;
        const auto& item = restore->items[restore->next];
        if (! chain.post ({ ChainCommandType::Insert, slot->id, (int) entries.size(), item.bypassed, slot.get() }))
            return;
        entries.push_back ({ slot->id, item.description.name, item.bypassed,
                             slot->processor.get(), item.description });
        slot.release();
        ++nextId;
        ++restore->next;
        ++restore->result.restored;
        scheduleRestore();
        changed();
        return;
    }

    if (restore->next == restore->items.size())
    {
        auto finished = std::move (restore);
        finished->result.failedNames.addArray (finished->overflowNames);
        if (finished->onDone)
            finished->onDone (std::move (finished->result));
        return;
    }

    // Create serially so completion order cannot reorder the XML. A factory
    // may complete inline; defer the next step to avoid recursive creation.
    restore->creating = true;
    const auto description = restore->items[restore->next].description;
    createProcessor (description,
        [weak = juce::WeakReference<PluginChain> (this), epoch = generation]
        (std::unique_ptr<juce::AudioProcessor> processor, const juce::String&)
        {
            auto* self = weak.get();
            if (self == nullptr || self->generation != epoch || ! self->restore)
                return;
            auto& job = *self->restore;
            const auto& item = job.items[job.next];
            if (processor != nullptr && setStereoLayout (*processor))
            {
                processor->setStateInformation (item.state.getData(), (int) item.state.getSize());
                auto slot = std::make_unique<ChainSlot>();
                slot->id = self->nextId;
                slot->bypassed = item.bypassed;
                slot->processor = std::move (processor);
                const auto spec = self->processingSpec();
                slot->prepare (spec.sampleRate, spec.maxBlockSize);
                job.pending = std::move (slot);
            }
            else
            {
                job.result.failedNames.add (item.description.name);
                ++job.next;
            }
            job.creating = false;
            self->scheduleRestore();
        });
}

void PluginChain::pumpRetired()
{
    JUCE_ASSERT_MESSAGE_THREAD
    std::vector<std::unique_ptr<ChainSlot>> toDelete;
    juce::StringArray dropped;

    while (auto* raw = chain.popRetired())
    {
        toDelete.emplace_back (raw);
        const int index = indexOf (raw->id);

        // Defensive: the audio side only retires an Insert the model should
        // never have made (a full chain). Drop the entry so no pointer to a
        // deleted processor stays in the model.
        if (index >= 0)
        {
            dropped.add (entries[(size_t) index].name);
            entries.erase (entries.begin() + index);
        }
    }

    // Announce the dropped entries while their processors still exist, so the
    // UI can close their editors first (an editor must not outlive its
    // processor). The slots are deleted when toDelete goes out of scope.
    if (! dropped.isEmpty())
    {
        changed();

        for (const auto& name : dropped)
            error (utf8 ("오디오 엔진이 체인에 넣지 못해 플러그인을 제거했습니다: ") + name);
    }
}

void PluginChain::timerCallback()
{
    const auto weak = juce::WeakReference<PluginChain> (this);
    pumpRetired();
    if (auto* self = weak.get())
        self->advanceRestore();
}

int PluginChain::indexOf (int id) const
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == id)
            return (int) i;

    return -1;
}

void PluginChain::changed()
{
    if (onChanged)
        onChanged();
}

void PluginChain::error (const juce::String& message)
{
    if (onError)
        onError (message);
}
