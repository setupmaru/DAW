#include "ChainProcessor.h"

//==============================================================================
void ChainSlot::prepare (double sampleRate, int maxBlockSize)
{
    jassert (processor != nullptr);
    jassert (sampleRate > 0.0 && maxBlockSize > 0);

    processor->setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
    processor->prepareToPlay (sampleRate, maxBlockSize);

    // Plugins commonly report their latency from prepareToPlay, so read it after.
    latencySamples = juce::jmax (0, processor->getLatencySamples());

    dryDelay.setMaximumDelayInSamples (latencySamples);
    dryDelay.prepare ({ sampleRate, (juce::uint32) maxBlockSize, 2 });
    dryDelay.setDelay ((float) latencySamples);
    dryDelay.reset();

    wet.reset (sampleRate, ChainProcessor::kSlotFadeSeconds);
    wet.setCurrentAndTargetValue (targetWet());

    // The plugin sees a buffer with as many channels as it has (at least the 2
    // we feed it); extra channels (e.g. a sidechain) are kept silent.
    const int numChannels = juce::jmax (2, processor->getTotalNumInputChannels(),
                                        processor->getTotalNumOutputChannels());
    processBuffer.setSize (numChannels, maxBlockSize);
    processBuffer.clear();

    midi.ensureSize (2048);
    midi.clear();

    preparedBlockSize = maxBlockSize;
}

//==============================================================================
ChainProcessor::~ChainProcessor()
{
    for (int i = 0; i < numSlots; ++i)
        delete slots[(size_t) i];

    ChainCommand cmd { ChainCommandType::Insert };
    while (commands.pop (cmd))
        if (cmd.type == ChainCommandType::Insert)
            delete cmd.slot;

    ChainSlot* slot = nullptr;
    while (retired.pop (slot))
        delete slot;
}

void ChainProcessor::prepare (double sampleRate, int maxBlockSize)
{
    jassert (sampleRate > 0.0 && maxBlockSize > 0);

    preparedSampleRate = sampleRate;
    preparedBlockSize = maxBlockSize;

    // TODO(Task 3): re-prepare every slot the chain owns (slot->prepare) so a
    // device change keeps the chain.
}

bool ChainProcessor::post (const ChainCommand& cmd)
{
    return commands.push (cmd);
}

ChainSlot* ChainProcessor::popRetired()
{
    ChainSlot* slot = nullptr;
    return retired.pop (slot) ? slot : nullptr;
}

int ChainProcessor::getTotalLatencySamples() const
{
    return totalLatencySamples.load (std::memory_order_relaxed);
}

int ChainProcessor::getNumActiveSlots() const
{
    return numActiveSlots.load (std::memory_order_relaxed);
}

//==============================================================================
void ChainProcessor::process (juce::AudioBuffer<float>& stereo)
{
    // ① Apply every queued command.
    ChainCommand cmd { ChainCommandType::Insert };
    while (commands.pop (cmd))
        applyCommand (cmd);

    publishState();

    if (numSlots == 0 || stereo.getNumChannels() < 2)
        return;

    // ② Run the slots in order. A block larger than a slot was prepared for is
    // split, so the slot's preallocated buffers are always big enough.
    const int totalSamples = stereo.getNumSamples();

    for (int i = 0; i < numSlots; ++i)
    {
        auto& slot = *slots[(size_t) i];

        if (slot.preparedBlockSize <= 0)
            continue;   // never prepared: leave the signal untouched

        for (int start = 0; start < totalSamples; start += slot.preparedBlockSize)
            processSlot (slot, stereo, start, juce::jmin (slot.preparedBlockSize, totalSamples - start));
    }
}

void ChainProcessor::applyCommand (const ChainCommand& cmd) noexcept
{
    switch (cmd.type)
    {
        case ChainCommandType::Insert:
            insertSlot (cmd.slot, cmd.index);
            break;

        case ChainCommandType::SetBypass:
        {
            const int index = findSlotIndex (cmd.slotId);
            if (index < 0)
                break;

            auto& slot = *slots[(size_t) index];
            slot.bypassed = cmd.bypassed;
            slot.wet.setTargetValue (slot.targetWet());
            break;
        }

        case ChainCommandType::Remove:
        case ChainCommandType::Move:
            // TODO(Task 3): set slot.pendingOp (and pendingIndex for Move), then
            // set slot.wet's target to slot.targetWet() (= 0). Once wet reaches 0,
            // apply the structural change (Remove -> push to `retired`, Move ->
            // reorder) and clear pendingOp. Ignored for now.
            break;
    }
}

void ChainProcessor::insertSlot (ChainSlot* slot, int index) noexcept
{
    if (slot == nullptr)
        return;

    if (numSlots >= kMaxSlots)
    {
        // Never write past the fixed array. Hand the slot straight back so the
        // message thread frees it. TODO(Task 3): retry next block if `retired`
        // is full instead of dropping it.
        retired.push (slot);
        return;
    }

    index = juce::jlimit (0, numSlots, index);

    for (int i = numSlots; i > index; --i)
        slots[(size_t) i] = slots[(size_t) (i - 1)];

    slots[(size_t) index] = slot;
    ++numSlots;

    // Starts fully dry and fades towards its target, so inserting never clicks.
    slot->pendingOp = ChainSlot::PendingOp::None;
    slot->wet.setCurrentAndTargetValue (0.0f);
    slot->wet.setTargetValue (slot->targetWet());
}

int ChainProcessor::findSlotIndex (int slotId) const noexcept
{
    for (int i = 0; i < numSlots; ++i)
        if (slots[(size_t) i]->id == slotId)
            return i;

    return -1;
}

void ChainProcessor::processSlot (ChainSlot& slot, juce::AudioBuffer<float>& stereo,
                                  int startSample, int numSamples) noexcept
{
    auto& buffer = slot.processBuffer;
    const int numChannels = buffer.getNumChannels();

    // Copy the input into the slot's preallocated buffer; extra channels stay silent.
    for (int ch = 0; ch < 2; ++ch)
        buffer.copyFrom (ch, 0, stereo, ch, startSample, numSamples);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    // A view of exactly numSamples. AudioBuffer keeps up to 32 channel pointers
    // inline, so this does not allocate.
    juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), numChannels, numSamples);

    // The plugin keeps running while bypassed so its state stays continuous.
    slot.midi.clear();
    slot.processor->processBlock (view, slot.midi);

    // out = wet * processed + (1 - wet) * dry, with dry delayed by the plugin's
    // latency so the two line up during a crossfade.
    auto* outL = stereo.getWritePointer (0, startSample);
    auto* outR = stereo.getWritePointer (1, startSample);
    const auto* wetL = buffer.getReadPointer (0);
    const auto* wetR = buffer.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        slot.dryDelay.pushSample (0, outL[i]);
        slot.dryDelay.pushSample (1, outR[i]);
        const float dryL = slot.dryDelay.popSample (0);
        const float dryR = slot.dryDelay.popSample (1);

        const float w = slot.wet.getNextValue();
        outL[i] = w * wetL[i] + (1.0f - w) * dryL;
        outR[i] = w * wetR[i] + (1.0f - w) * dryR;
    }
}

void ChainProcessor::publishState() noexcept
{
    int latency = 0;
    for (int i = 0; i < numSlots; ++i)
        latency += slots[(size_t) i]->latencySamples;

    totalLatencySamples.store (latency, std::memory_order_relaxed);
    numActiveSlots.store (numSlots, std::memory_order_relaxed);
}
