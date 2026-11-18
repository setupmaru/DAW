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

//==============================================================================
PluginChain::PluginChain (ChainProcessor& c, juce::AudioPluginFormatManager& f)
    : chain (c), formatManager (f)
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

bool PluginChain::addProcessor (std::unique_ptr<juce::AudioProcessor> processor, const juce::String& name)
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
    slot->prepare (sampleRate, blockSize);

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
    entries.push_back ({ nextId++, name, false, rawProcessor });
    changed();
    return true;
}

void PluginChain::addPlugin (const juce::PluginDescription& description)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The callback runs on the message thread, possibly after this object is
    // gone; the weak reference catches that and the new instance is simply
    // deleted. A pending creation has no id yet, so remove() cannot target it.
    formatManager.createPluginInstanceAsync (
        description, sampleRate, blockSize,
        [weak = juce::WeakReference<PluginChain> (this), name = description.name]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& message)
        {
            auto* self = weak.get();

            if (self == nullptr)
                return;

            if (instance == nullptr)
            {
                self->error (utf8 ("플러그인을 불러오지 못했습니다: ") + name
                             + (message.isNotEmpty() ? " (" + message + ")" : juce::String()));
                return;
            }

            self->addProcessor (std::move (instance), name);
        });
}

bool PluginChain::remove (int id)
{
    JUCE_ASSERT_MESSAGE_THREAD
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

void PluginChain::pumpRetired()
{
    JUCE_ASSERT_MESSAGE_THREAD
    bool modelChanged = false;
    juce::StringArray dropped;

    while (auto* raw = chain.popRetired())
    {
        std::unique_ptr<ChainSlot> slot (raw);
        const int index = indexOf (slot->id);

        // Defensive: the audio side only retires an Insert the model should
        // never have made (a full chain). Drop the entry before its processor
        // is deleted so no dangling pointer stays in the model.
        if (index >= 0)
        {
            dropped.add (entries[(size_t) index].name);
            entries.erase (entries.begin() + index);
            modelChanged = true;
        }
    }

    // Callbacks only after the queue is drained and the model is consistent.
    if (modelChanged)
        changed();

    for (const auto& name : dropped)
        error (utf8 ("오디오 엔진이 체인에 넣지 못해 플러그인을 제거했습니다: ") + name);
}

void PluginChain::timerCallback()
{
    pumpRetired();
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
