#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "ChainSlot.h"
#include "LockFreeQueue.h"

#include <array>
#include <atomic>

enum class ChainCommandType { Insert, Remove, Move, SetBypass };

// Message thread -> audio thread. For Insert, `slot` is a prepared slot whose
// ownership passes to the ChainProcessor once post() returns true; the slot's
// own `bypassed` field is its initial bypass state.
struct ChainCommand
{
    ChainCommandType type;
    int slotId = 0;
    int index = 0;
    bool bypassed = false;
    ChainSlot* slot = nullptr;
};

// Real-time effect chain. Every change crossfades a slot between wet and dry,
// so nothing clicks while the chain is edited live.
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

    void prepare (double sampleRate, int maxBlockSize);   // only while audio is stopped
    void process (juce::AudioBuffer<float>& stereo);      // audio thread, 2 channels
    bool post (const ChainCommand&);                      // message thread; false when the queue is full
    ChainSlot* popRetired();                              // message thread; nullptr if none. Caller deletes
    int getTotalLatencySamples() const;
    int getNumActiveSlots() const;

private:
    void applyCommand (const ChainCommand&) noexcept;
    void insertSlot (ChainSlot* slot, int index) noexcept;
    int findSlotIndex (int slotId) const noexcept;
    void processSlot (ChainSlot& slot, juce::AudioBuffer<float>& stereo, int startSample, int numSamples) noexcept;
    void publishState() noexcept;

    LockFreeQueue<ChainCommand, kCommandQueueCapacity> commands;
    LockFreeQueue<ChainSlot*, kRetiredQueueCapacity> retired;

    // Audio-thread owned. Fixed size: no allocation when the chain changes.
    std::array<ChainSlot*, kMaxSlots> slots {};
    int numSlots = 0;

    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;

    std::atomic<int> totalLatencySamples { 0 };
    std::atomic<int> numActiveSlots { 0 };

    JUCE_DECLARE_NON_COPYABLE (ChainProcessor)
};
