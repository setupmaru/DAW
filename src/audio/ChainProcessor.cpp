#include "ChainProcessor.h"

#include <vector>

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

    if (hasDeferredCommand && deferredCommand.type == ChainCommandType::Insert)
        delete deferredCommand.slot;

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

    // Re-prepare every slot the chain owns, so a device change keeps the chain.
    // Audio is stopped, so no fades are needed: each slot jumps to its target
    // wet, and a slot that was waiting for the output dip simply joins.
    for (int i = 0; i < numSlots; ++i)
    {
        auto& slot = *slots[(size_t) i];
        slot.prepare (sampleRate, maxBlockSize);
        slot.awaitingDip = false;
    }

    // Slots still on their way in were prepared for the old device. Audio is
    // stopped, so this (message) thread may also act as the queue's consumer:
    // take every command out, re-prepare its slot, and put it back in order.
    if (hasDeferredCommand && deferredCommand.slot != nullptr)
        deferredCommand.slot->prepare (sampleRate, maxBlockSize);

    std::vector<ChainCommand> queued;
    queued.reserve ((size_t) commands.getNumReady());

    ChainCommand cmd { ChainCommandType::Insert };
    while (commands.pop (cmd))
        queued.push_back (cmd);

    for (const auto& c : queued)
    {
        if (c.type == ChainCommandType::Insert && c.slot != nullptr)
            c.slot->prepare (sampleRate, maxBlockSize);

        const bool pushed = commands.push (c);   // same count as just popped: cannot fail
        jassertquiet (pushed);
    }

    outputGain.reset (sampleRate, kOutputDipSeconds);
    outputGain.setCurrentAndTargetValue (1.0f);
    dipHoldSamples = 0;

    publishState();
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
    // ① Take in queued commands. ② Apply the structural changes that are ready
    // (their slot is dry and, if they change latency, the output is dipped).
    applyQueuedCommands();
    const bool wantDip = applyPendingChanges();
    outputGain.setTargetValue (wantDip ? 0.0f : 1.0f);

    publishState();

    if (stereo.getNumChannels() < 2)
        return;

    // ③ Run the slots in order. A block larger than a slot was prepared for is
    // split, so the slot's preallocated buffers are always big enough.
    const int totalSamples = stereo.getNumSamples();

    for (int i = 0; i < numSlots; ++i)
    {
        auto& slot = *slots[(size_t) i];

        if (slot.preparedBlockSize <= 0 || slot.awaitingDip)
            continue;   // never prepared, or not in the signal path yet: leave the signal untouched

        for (int start = 0; start < totalSamples; start += slot.preparedBlockSize)
            processSlot (slot, stereo, start, juce::jmin (slot.preparedBlockSize, totalSamples - start));
    }

    // ④ Output dip.
    applyOutputGain (stereo);
}

void ChainProcessor::applyQueuedCommands() noexcept
{
    if (hasDeferredCommand)
    {
        if (! applyCommand (deferredCommand))
            return;   // still stuck: later commands keep waiting in the queue

        hasDeferredCommand = false;
    }

    ChainCommand cmd { ChainCommandType::Insert };
    while (commands.pop (cmd))
    {
        if (! applyCommand (cmd))
        {
            deferredCommand = cmd;
            hasDeferredCommand = true;
            return;
        }
    }
}

bool ChainProcessor::applyCommand (const ChainCommand& cmd) noexcept
{
    if (cmd.type == ChainCommandType::Insert)
    {
        if (cmd.slot != nullptr)
            cmd.slot->bypassed = cmd.bypassed;

        return insertSlot (cmd.slot, cmd.index);
    }

    const int index = findSlotIndex (cmd.slotId);
    if (index < 0)
        return true;   // unknown (or already removed) slot: nothing to do

    auto& slot = *slots[(size_t) index];

    switch (cmd.type)
    {
        case ChainCommandType::SetBypass:
            slot.bypassed = cmd.bypassed;
            break;

        case ChainCommandType::Remove:
            slot.pendingOp = ChainSlot::PendingOp::Remove;
            break;

        case ChainCommandType::Move:
        {
            if (slot.pendingOp == ChainSlot::PendingOp::Remove)
                break;   // being removed: the move no longer matters

            if (juce::jlimit (0, numSlots - 1, cmd.index) == index)
            {
                // Already there: nothing to move (cancels an earlier pending move).
                slot.pendingOp = ChainSlot::PendingOp::None;
                break;
            }

            slot.pendingOp = ChainSlot::PendingOp::Move;
            slot.pendingIndex = cmd.index;
            break;
        }

        case ChainCommandType::Insert:
            break;   // handled above
    }

    retarget (slot);   // a pending op forces the target to 0 (ChainSlot::targetWet)
    return true;
}

bool ChainProcessor::insertSlot (ChainSlot* slot, int index) noexcept
{
    if (slot == nullptr)
        return true;

    if (numSlots >= kMaxSlots)
    {
        // Never write past the fixed array: hand the slot straight back so the
        // message thread frees it. If the retired queue is full, report failure
        // so the command is retried next block and the slot is not lost.
        return retired.push (slot);
    }

    index = juce::jlimit (0, numSlots, index);

    for (int i = numSlots; i > index; --i)
        slots[(size_t) i] = slots[(size_t) (i - 1)];

    slots[(size_t) index] = slot;
    ++numSlots;

    // Starts fully dry and fades towards its target, so inserting never clicks.
    // A slot with latency would also change the delay of the signal path, so it
    // waits outside the signal path until the output has dipped to 0.
    slot->pendingOp = ChainSlot::PendingOp::None;
    slot->awaitingDip = slot->latencySamples > 0;
    slot->wet.setCurrentAndTargetValue (0.0f);
    retarget (*slot);
    return true;
}

bool ChainProcessor::applyPendingChanges() noexcept
{
    const bool dipped = outputIsDipped();
    bool wantDip = false;

    // Lowest index from which the signal path's delay changed at gain 0.
    bool latencyChanged = false;
    int changedFrom = 0;

    auto noteLatencyChange = [&] (int index)
    {
        changedFrom = latencyChanged ? juce::jmin (changedFrom, index) : index;
        latencyChanged = true;
    };

    for (int i = 0; i < numSlots;)
    {
        auto& slot = *slots[(size_t) i];

        // Removing or moving a slot that is in the signal path with latency
        // changes the delay at and after its position.
        const bool changesLatency = ! slot.awaitingDip && slot.latencySamples > 0;

        if (slot.pendingOp == ChainSlot::PendingOp::Remove)
        {
            // Wait (in the chain, dry, without dipping) until the retired queue
            // has room, so the slot is never lost.
            if (! retiredHasRoom())
            {
                ++i;
                continue;
            }

            if (changesLatency)
                wantDip = true;

            if (! isDry (slot) || (changesLatency && ! dipped) || ! retired.push (&slot))
            {
                ++i;
                continue;
            }

            for (int j = i; j < numSlots - 1; ++j)
                slots[(size_t) j] = slots[(size_t) (j + 1)];

            slots[(size_t) --numSlots] = nullptr;

            if (changesLatency)
                noteLatencyChange (i);

            continue;   // slots[i] is now the next slot
        }

        if (slot.pendingOp == ChainSlot::PendingOp::Move)
        {
            if (changesLatency)
                wantDip = true;

            if (! isDry (slot) || (changesLatency && ! dipped))
            {
                ++i;
                continue;
            }

            const int to = juce::jlimit (0, numSlots - 1, slot.pendingIndex);
            slot.pendingOp = ChainSlot::PendingOp::None;
            moveSlot (i, to);
            retarget (slot);   // back to wet (or stays dry if bypassed)

            if (changesLatency)
                noteLatencyChange (juce::jmin (i, to));

            i = 0;   // indices shifted: rescan. Bounded, as every rescan clears one pending op.
            continue;
        }

        if (slot.awaitingDip)
        {
            wantDip = true;

            if (dipped)
            {
                slot.awaitingDip = false;   // joins the signal path, fully dry, and fades in
                retarget (slot);
                noteLatencyChange (i);
            }
        }

        ++i;
    }

    // Keep the output silent until the change has passed through every slot
    // with latency at and after where it happened (including a newly inserted
    // slot, whose own delay starts empty).
    if (latencyChanged)
        dipHoldSamples = juce::jmax (dipHoldSamples, latencyFrom (changedFrom));

    return wantDip;
}

bool ChainProcessor::retiredHasRoom() const noexcept
{
    // The audio thread is the only producer, so room can only grow until it pushes.
    return retired.getNumReady() < kRetiredQueueCapacity;
}

bool ChainProcessor::isDry (const ChainSlot& slot) const noexcept
{
    if (slot.awaitingDip || slot.preparedBlockSize <= 0)
        return true;   // not processed at all

    return ! slot.wet.isSmoothing() && juce::exactlyEqual (slot.wet.getCurrentValue(), 0.0f);
}

bool ChainProcessor::outputIsDipped() const noexcept
{
    return dipHoldSamples > 0
        || (! outputGain.isSmoothing() && juce::exactlyEqual (outputGain.getCurrentValue(), 0.0f));
}

int ChainProcessor::latencyFrom (int index) const noexcept
{
    int latency = 0;
    for (int i = juce::jmax (0, index); i < numSlots; ++i)
        if (! slots[(size_t) i]->awaitingDip)
            latency += slots[(size_t) i]->latencySamples;

    return latency;
}

void ChainProcessor::moveSlot (int from, int to) noexcept
{
    auto* slot = slots[(size_t) from];

    if (from < to)
        for (int i = from; i < to; ++i)
            slots[(size_t) i] = slots[(size_t) (i + 1)];
    else
        for (int i = from; i > to; --i)
            slots[(size_t) i] = slots[(size_t) (i - 1)];

    slots[(size_t) to] = slot;
}

void ChainProcessor::retarget (ChainSlot& slot) noexcept
{
    // A slot outside the signal path is not processed, so its wet must not
    // start a ramp it could never finish; it is retargeted when it joins.
    if (! slot.awaitingDip)
        slot.wet.setTargetValue (slot.targetWet());
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

void ChainProcessor::applyOutputGain (juce::AudioBuffer<float>& stereo) noexcept
{
    if (dipHoldSamples == 0 && ! outputGain.isSmoothing()
        && juce::exactlyEqual (outputGain.getCurrentValue(), 1.0f))
        return;   // no dip in progress: leave the output bit-exact

    auto* outL = stereo.getWritePointer (0);
    auto* outR = stereo.getWritePointer (1);

    for (int i = 0; i < stereo.getNumSamples(); ++i)
    {
        float g = 0.0f;

        if (dipHoldSamples > 0)
            --dipHoldSamples;   // silent; the ramp back up waits until the hold is over
        else
            g = outputGain.getNextValue();

        outL[i] *= g;
        outR[i] *= g;
    }
}

void ChainProcessor::publishState() noexcept
{
    int latency = 0;
    for (int i = 0; i < numSlots; ++i)
        if (! slots[(size_t) i]->awaitingDip)
            latency += slots[(size_t) i]->latencySamples;

    totalLatencySamples.store (latency, std::memory_order_relaxed);
    numActiveSlots.store (numSlots, std::memory_order_relaxed);
}
