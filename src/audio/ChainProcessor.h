#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "ChainSlot.h"
#include "LockFreeQueue.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

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
//  - Insert/Move indices are positions in the live chain, the slots not being
//    removed, exactly as the message thread's model numbers them. Moves and
//    inserts end in the model's order even while other slots are fading.
//  - The kMaxSlots limit counts live slots. An Insert into a chain with
//    kMaxSlots live slots is handed straight back through popRetired(). If the
//    retired queue is full, or too many slots are still fading out, the slot
//    (or the command) waits and is retried every block: no slot is ever lost.
//
// Threads: post/popRetired on the message thread, process on the audio thread.
// prepare is message-thread only. Only setProcessingActive(false) may run on a
// driver thread, with IO stopped. A control mutex serialises it with edits;
// process never takes it. Plugin callbacks must defer calls into this class
// (e.g. with AsyncUpdater): prepareToPlay can synchronously notify listeners
// while prepare holds the non-recursive control mutex.
// process() and everything it calls never allocates, frees, locks or logs.
class ChainProcessor
{
public:
    static constexpr int kMaxSlots = 16;
    static constexpr double kSlotFadeSeconds = 0.020, kOutputDipSeconds = 0.010;
    static constexpr int kCommandQueueCapacity = 64;
    static constexpr int kRetiredQueueCapacity = 64;

    ChainProcessor() = default;
    ~ChainProcessor();   // deletes every slot it still owns (call with audio stopped)

    // Message thread only, while IO is stopped. Re-prepares every slot,
    // including Inserts still waiting in the queue.
    void prepare (double sampleRate, int maxBlockSize);
    struct ProcessingSpec { double sampleRate; int maxBlockSize; };
    ProcessingSpec getProcessingSpec() const;             // { 0, 0 } before prepare()

    // Only while IO is stopped, before its first block / after its last block.
    // Stopping settles all edits without running plugins; further posts apply
    // immediately until restarted. Removed slots still await popRetired(), so
    // editors close on the message thread before processors are destroyed.
    void setProcessingActive (bool active);
    void process (juce::AudioBuffer<float>& stereo);      // audio thread, 2 channels
    bool post (const ChainCommand&);                      // message thread; false when the queue is full
    ChainSlot* popRetired();                              // message thread; nullptr if none. Caller deletes
    int getTotalLatencySamples() const;
    int getNumActiveSlots() const;                        // live slots (not being removed)

private:
    void settleStoppedChanges();                          // control mutex held, IO stopped
    void collectStoppedRetired();                         // preserves slots for the message thread
    void applyQueuedCommands() noexcept;
    bool applyCommand (const ChainCommand&) noexcept;   // false: retry this command next block
    bool insertSlot (ChainSlot* slot, int index) noexcept;
    bool applyPendingChanges() noexcept;                // returns true while an output dip is wanted
    bool retiredHasRoom() const noexcept;
    bool isDry (const ChainSlot&) const noexcept;
    bool outputIsDipped() const noexcept;
    int latencyFrom (int index) const noexcept;
    int placeSlot (int from) noexcept;
    bool isInPlace (const ChainSlot&) const noexcept;
    ChainSlot* nextSettledAfter (int logicalIndex) const noexcept;
    static void moveWithin (ChainSlot** array, int from, int to) noexcept;
    void retarget (ChainSlot&) noexcept;
    int findLiveIndex (int slotId) const noexcept;
    int logicalIndexOf (const ChainSlot*) const noexcept;
    int physicalIndexOf (const ChainSlot*) const noexcept;
    void processSlot (ChainSlot& slot, juce::AudioBuffer<float>& stereo, int startSample, int numSamples) noexcept;
    void applyOutputGain (juce::AudioBuffer<float>& stereo) noexcept;
    void publishState() noexcept;

    LockFreeQueue<ChainCommand, kCommandQueueCapacity> commands;
    LockFreeQueue<ChainSlot*, kRetiredQueueCapacity> retired;

    mutable std::mutex controlMutex;
    bool processingActive = true;                        // standalone users drive process() themselves
    std::vector<ChainSlot*> stoppedRetired;               // only outside the IO callback

    // Audio-thread owned. Fixed size: no allocation when the chain changes.
    // `slots` is what is processed: live slots plus slots fading out before
    // removal, hence the headroom. `order` is the live chain in model order;
    // command indices and the kMaxSlots limit refer to it. See placeSlot().
    static constexpr int kMaxPhysicalSlots = 2 * kMaxSlots;
    std::array<ChainSlot*, kMaxPhysicalSlots> slots {};
    int numSlots = 0;
    std::array<ChainSlot*, kMaxSlots> order {};
    int numLive = 0;

    // A command that could not be applied yet (an Insert into a full chain
    // while the retired queue is full, or while `slots` is full of slots
    // fading out). Retried first next block; later
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
