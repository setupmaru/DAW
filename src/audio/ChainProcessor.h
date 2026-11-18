#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "ChainSlot.h"
#include "LockFreeQueue.h"

#include <array>
#include <atomic>

enum class ChainCommandType { Insert, Remove, Move, SetBypass };

// Message thread -> audio thread. For Insert, `slot` is a prepared slot whose
// ownership passes to the ChainProcessor once post() returns true, and
// `bypassed` is its initial bypass state (it overwrites slot->bypassed).
// For Move, `index` is the destination (clamped to the chain).
struct ChainCommand
{
    ChainCommandType type;
    int slotId = 0;
    int index = 0;
    bool bypassed = false;
    ChainSlot* slot = nullptr;
};

// Real-time effect chain. Every change crossfades a slot between wet and dry,
// so nothing clicks while the chain is edited live:
//  - Insert: the slot goes in fully dry, then fades to wet.
//  - Remove: the slot fades to dry, then leaves the chain through popRetired().
//  - Move:   the slot fades to dry, moves, then fades back.
//  - A change that alters the latency of the signal path (adding, removing or
//    moving a slot with latency) also dips the whole output to 0 over
//    kOutputDipSeconds, applies the change at 0, keeps the output silent until
//    the change has passed through every latency slot behind it, then fades
//    back in. Without this the delay length would jump (click) or the signal
//    would drop out while a new delay line fills.
//  - An Insert into a full chain (kMaxSlots) is handed straight back through
//    popRetired(). If the retired queue is full, the slot waits in the chain
//    (or the command waits) and is retried every block: no slot is ever lost.
//
// Threads: prepare/post/popRetired on the message thread, process on the audio
// thread. process() and everything it calls never allocates, frees, locks or logs.
class ChainProcessor
{
public:
    static constexpr int kMaxSlots = 16;
    static constexpr double kSlotFadeSeconds = 0.020, kOutputDipSeconds = 0.010;
    static constexpr int kCommandQueueCapacity = 64;
    static constexpr int kRetiredQueueCapacity = 64;

    ChainProcessor() = default;
    ~ChainProcessor();   // deletes every slot it still owns (call with audio stopped)

    // Only while audio is stopped, and never concurrently with post() (call it
    // on the message thread): it re-prepares every slot the chain owns, including
    // slots still waiting in the command queue, which it briefly drains and refills.
    void prepare (double sampleRate, int maxBlockSize);
    void process (juce::AudioBuffer<float>& stereo);      // audio thread, 2 channels
    bool post (const ChainCommand&);                      // message thread; false when the queue is full
    ChainSlot* popRetired();                              // message thread; nullptr if none. Caller deletes
    int getTotalLatencySamples() const;
    int getNumActiveSlots() const;

private:
    void applyQueuedCommands() noexcept;
    bool applyCommand (const ChainCommand&) noexcept;   // false: retry this command next block
    bool insertSlot (ChainSlot* slot, int index) noexcept;
    bool applyPendingChanges() noexcept;                // returns true while an output dip is wanted
    bool retiredHasRoom() const noexcept;
    bool isDry (const ChainSlot&) const noexcept;
    bool outputIsDipped() const noexcept;
    int latencyFrom (int index) const noexcept;
    void moveSlot (int from, int to) noexcept;
    void retarget (ChainSlot&) noexcept;
    int findSlotIndex (int slotId) const noexcept;
    void processSlot (ChainSlot& slot, juce::AudioBuffer<float>& stereo, int startSample, int numSamples) noexcept;
    void applyOutputGain (juce::AudioBuffer<float>& stereo) noexcept;
    void publishState() noexcept;

    LockFreeQueue<ChainCommand, kCommandQueueCapacity> commands;
    LockFreeQueue<ChainSlot*, kRetiredQueueCapacity> retired;

    // Audio-thread owned. Fixed size: no allocation when the chain changes.
    std::array<ChainSlot*, kMaxSlots> slots {};
    int numSlots = 0;

    // A command that could not be applied yet (an Insert into a full chain
    // while the retired queue is full). Retried first next block; later
    // commands wait behind it in the queue so their order is kept.
    ChainCommand deferredCommand { ChainCommandType::Insert };
    bool hasDeferredCommand = false;

    // Output dip for latency changes: whole-output gain, plus a count of samples
    // to keep silent after a change was applied at gain 0.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGain { 1.0f };
    int dipHoldSamples = 0;

    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;

    std::atomic<int> totalLatencySamples { 0 };
    std::atomic<int> numActiveSlots { 0 };

    JUCE_DECLARE_NON_COPYABLE (ChainProcessor)
};
