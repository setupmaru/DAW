#include "TestProcessors.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr float kClickThreshold = 0.06f;
constexpr float kTolerance = 1.0e-5f;

void append (std::vector<float>& dest, const std::vector<float>& src)
{
    dest.insert (dest.end(), src.begin(), src.end());
}

// out = in + offset. Does not commute with a gain, so it shows the chain order.
class OffsetProcessor : public TestProcessorBase
{
public:
    explicit OffsetProcessor (float o) : offset (o) {}

    const juce::String getName() const override { return "Offset"; }

    using TestProcessorBase::processBlock;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] += offset;
        }
    }

private:
    float offset;
};
} // namespace

struct ChainProcessorTests : public juce::UnitTest
{
    ChainProcessorTests() : juce::UnitTest ("ChainProcessorTests", "DAW") {}

    void runTest() override
    {
        testEmptyChainPassesThrough();
        testInsertFadesInWithoutClick();
        testBypassToggleWithoutClick();
        testRapidBypassToggleNoClick();
        testRemoveWithoutClickAndRetires();
        testMoveWithoutClick();
        testMoveChangesOrder();
        testLatencyChangeUsesDipWithoutClick();
        testLatencyChangeBehindLatencySlotNoClick();
        testBypassedLatencySlotDelaysDryPath();
        testInsertHonoursBypassedFlag();
        testSeventeenthInsertIsRetired();
        testFullRetiredQueueKeepsSlots();
        testReprepareKeepsSlots();
        testMoveAfterRemoveUsesModelIndex();
        testInsertAfterRemoveUsesModelIndex();
        testMovesApplyInCommandOrder();
        testInsertDuringRemoveFadeIsNotRetired();
        testPhysicalArrayFullDefersInsert();
    }

    //==========================================================================
    bool postInsert (ChainProcessor& chain, int id, std::unique_ptr<juce::AudioProcessor> processor,
                     int index = 0, bool bypassed = false)
    {
        auto slot = makeSlot (id, std::move (processor), kSampleRate, kBlock);
        ChainCommand cmd { ChainCommandType::Insert };
        cmd.slotId = id;
        cmd.index = index;
        cmd.bypassed = bypassed;
        cmd.slot = slot.get();

        if (! chain.post (cmd))
            return false;

        slot.release();   // the chain owns it now
        return true;
    }

    bool postBypass (ChainProcessor& chain, int id, bool bypassed)
    {
        ChainCommand cmd { ChainCommandType::SetBypass };
        cmd.slotId = id;
        cmd.bypassed = bypassed;
        return chain.post (cmd);
    }

    bool postRemove (ChainProcessor& chain, int id)
    {
        ChainCommand cmd { ChainCommandType::Remove };
        cmd.slotId = id;
        return chain.post (cmd);
    }

    bool postMove (ChainProcessor& chain, int id, int index)
    {
        ChainCommand cmd { ChainCommandType::Move };
        cmd.slotId = id;
        cmd.index = index;
        return chain.post (cmd);
    }

    // Pops (and frees) every retired slot, returning their ids in pop order.
    std::vector<int> drainRetired (ChainProcessor& chain)
    {
        std::vector<int> ids;
        while (auto* raw = chain.popRetired())
        {
            std::unique_ptr<ChainSlot> slot (raw);
            ids.push_back (slot->id);
        }
        return ids;
    }

    // Runs one block and checks out[n] == expected(n) for every sample of it,
    // where n is the absolute sample index of the input signal.
    void expectLastBlockMatches (ChainProcessor& chain, const std::function<float (std::int64_t)>& expected,
                                 const juce::String& what)
    {
        const auto start = sinePosition (chain);
        const auto out = runSine (chain, 1);

        float worst = 0.0f;
        for (size_t i = 0; i < out.size(); ++i)
            worst = juce::jmax (worst, std::abs (out[i] - expected (start + (std::int64_t) i)));

        expectLessThan (worst, kTolerance, what);
    }

    // Runs one block and checks out == gain * in for every sample of it.
    void expectLastBlockIs (ChainProcessor& chain, float gain, const juce::String& what)
    {
        expectLastBlockMatches (chain, [gain] (std::int64_t n) { return gain * sineSample (n); }, what);
    }

    // Runs one block and checks out[n] == in[n - delay] for every sample of it.
    void expectLastBlockIsDelayed (ChainProcessor& chain, int delay, const juce::String& what)
    {
        expectLastBlockMatches (chain, [delay] (std::int64_t n) { return sineSample (n - delay); }, what);
    }

    //==========================================================================
    void testEmptyChainPassesThrough()
    {
        beginTest ("testEmptyChainPassesThrough");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        const auto start = sinePosition (chain);
        const auto out = runSine (chain, 8);

        int mismatches = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (! juce::exactlyEqual (out[i], sineSample (start + (std::int64_t) i)))
                ++mismatches;

        expectEquals (mismatches, 0, "empty chain must output its input bit-exactly");
        expectEquals (chain.getNumActiveSlots(), 0);
        expectEquals (chain.getTotalLatencySamples(), 0);
    }

    void testInsertFadesInWithoutClick()
    {
        beginTest ("testInsertFadesInWithoutClick");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        auto out = runSine (chain, 10);
        expect (postInsert (chain, 1, std::make_unique<InvertProcessor>()));
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold, "insert must fade in without a click");
        expectLastBlockIs (chain, -1.0f, "after the fade-in, out == -in");
        expectEquals (chain.getNumActiveSlots(), 1);
    }

    void testBypassToggleWithoutClick()
    {
        beginTest ("testBypassToggleWithoutClick");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<InvertProcessor>()));
        auto out = runSine (chain, 20);
        expectLastBlockIs (chain, -1.0f, "inserted invert is settled");

        out = runSine (chain, 2);
        expect (postBypass (chain, 1, true));
        append (out, runSine (chain, 20));
        expectLessThan (maxStep (out), kClickThreshold, "bypass on must crossfade without a click");
        expectLastBlockIs (chain, 1.0f, "bypassed slot outputs the dry input");

        out = runSine (chain, 2);
        expect (postBypass (chain, 1, false));
        append (out, runSine (chain, 20));
        expectLessThan (maxStep (out), kClickThreshold, "bypass off must crossfade without a click");
        expectLastBlockIs (chain, -1.0f, "un-bypassed slot is wet again");
    }

    void testRapidBypassToggleNoClick()
    {
        beginTest ("testRapidBypassToggleNoClick");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<InvertProcessor>()));
        auto out = runSine (chain, 20);

        bool bypassed = false;
        for (int i = 0; i < 50; ++i)
        {
            bypassed = ! bypassed;
            expect (postBypass (chain, 1, bypassed));
            append (out, runSine (chain, 1));
        }

        expect (! bypassed, "50 toggles end where they started");
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold, "rapid bypass toggling must not click");
        expectLastBlockIs (chain, -1.0f, "settles back to the original (wet) state");
    }

    //==========================================================================
    void testRemoveWithoutClickAndRetires()
    {
        beginTest ("testRemoveWithoutClickAndRetires");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<InvertProcessor>()));
        runSine (chain, 20);
        expectLastBlockIs (chain, -1.0f, "inserted invert is settled");

        auto out = runSine (chain, 2);
        expect (postRemove (chain, 1));
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold, "remove must fade out without a click");
        expectLastBlockIs (chain, 1.0f, "after the remove, out == in");
        expectEquals (chain.getNumActiveSlots(), 0);

        std::unique_ptr<ChainSlot> retiredSlot (chain.popRetired());
        expect (retiredSlot != nullptr, "the removed slot comes back through popRetired");
        if (retiredSlot != nullptr)
            expectEquals (retiredSlot->id, 1);

        expect (chain.popRetired() == nullptr, "only one slot is retired");
    }

    void testMoveWithoutClick()
    {
        beginTest ("testMoveWithoutClick");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<GainProcessor> (0.5f), 0));
        expect (postInsert (chain, 2, std::make_unique<InvertProcessor>(), 1));
        runSine (chain, 20);
        expectLastBlockIs (chain, -0.5f, "gain + invert settled");

        auto out = runSine (chain, 2);
        expect (postMove (chain, 2, 0));
        append (out, runSine (chain, 30));

        expectLessThan (maxStep (out), kClickThreshold, "move must fade out, reorder and fade in without a click");
        expectLastBlockIs (chain, -0.5f, "after the move, out == -0.5 * in");
        expectEquals (chain.getNumActiveSlots(), 2);
        expect (chain.popRetired() == nullptr, "a move retires nothing");
    }

    void testMoveChangesOrder()
    {
        beginTest ("testMoveChangesOrder");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        // gain then offset: out = 0.5 * in + 0.6. The orders differ by 0.3, so an
        // unfaded reorder would step by 0.3, five times the click threshold.
        expect (postInsert (chain, 1, std::make_unique<GainProcessor> (0.5f), 0));
        expect (postInsert (chain, 2, std::make_unique<OffsetProcessor> (0.6f), 1));
        runSine (chain, 20);
        expectLastBlockMatches (chain, [] (std::int64_t n) { return 0.5f * sineSample (n) + 0.6f; },
                                "gain -> offset");

        // offset then gain: out = 0.5 * (in + 0.6)
        auto out = runSine (chain, 2);
        expect (postMove (chain, 2, 0));
        append (out, runSine (chain, 30));

        expectLessThan (maxStep (out), kClickThreshold, "move must not click");
        expectLastBlockMatches (chain, [] (std::int64_t n) { return 0.5f * (sineSample (n) + 0.6f); },
                                "offset -> gain after the move");

        // and back again, with an out-of-range index (clamped to the end)
        out = runSine (chain, 2);
        expect (postMove (chain, 2, 99));
        append (out, runSine (chain, 30));

        expectLessThan (maxStep (out), kClickThreshold, "move back must not click");
        expectLastBlockMatches (chain, [] (std::int64_t n) { return 0.5f * sineSample (n) + 0.6f; },
                                "gain -> offset after moving back");
    }

    void testLatencyChangeUsesDipWithoutClick()
    {
        beginTest ("testLatencyChangeUsesDipWithoutClick");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        auto out = runSine (chain, 10);
        expect (postInsert (chain, 1, std::make_unique<FixedDelayProcessor> (480)));
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold,
                        "inserting a latency slot must dip the output, with no click and no hard dropout");
        expectEquals (chain.getTotalLatencySamples(), 480);
        expectLastBlockIsDelayed (chain, 480, "after the insert, out[n] == in[n - 480]");

        out = runSine (chain, 2);
        expect (postRemove (chain, 1));
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold, "removing a latency slot must dip the output without a click");
        expectEquals (chain.getTotalLatencySamples(), 0);
        expectLastBlockIs (chain, 1.0f, "after the remove, out == in");
        expectEquals ((int) drainRetired (chain).size(), 1);
    }

    // The dip must stay silent until the change has passed through every
    // latency slot behind it.
    void testLatencyChangeBehindLatencySlotNoClick()
    {
        beginTest ("testLatencyChangeBehindLatencySlotNoClick");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<FixedDelayProcessor> (240)));
        runSine (chain, 40);
        expectLastBlockIsDelayed (chain, 240, "first delay settled");

        auto out = runSine (chain, 2);
        expect (postInsert (chain, 2, std::make_unique<FixedDelayProcessor> (480), 0));
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold, "inserting in front of a latency slot must not click");
        expectEquals (chain.getTotalLatencySamples(), 720);
        expectLastBlockIsDelayed (chain, 720, "both delays in the chain");

        out = runSine (chain, 2);
        expect (postRemove (chain, 2));
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold, "removing in front of a latency slot must not click");
        expectEquals (chain.getTotalLatencySamples(), 240);
        expectLastBlockIsDelayed (chain, 240, "only the first delay left");

        out = runSine (chain, 2);
        expect (postMove (chain, 1, 0));   // already at 0: nothing to do, no dip
        append (out, runSine (chain, 10));
        expectLessThan (maxStep (out), kClickThreshold, "no-op move must not click");
        expectLastBlockIsDelayed (chain, 240, "no-op move keeps the delay");

        expectEquals ((int) drainRetired (chain).size(), 1);
    }

    void testBypassedLatencySlotDelaysDryPath()
    {
        beginTest ("testBypassedLatencySlotDelaysDryPath");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        auto out = runSine (chain, 10);
        expect (postInsert (chain, 1, std::make_unique<FixedDelayProcessor> (480), 0, true));
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold, "inserting a bypassed latency slot must not click");
        expectEquals (chain.getTotalLatencySamples(), 480);
        expectLastBlockIsDelayed (chain, 480, "bypassed latency slot: out[n] == in[n - 480] (dry path delayed)");

        // Wet and delayed dry line up exactly, so every sample through both
        // crossfades is in[n - 480]: no click and no comb filtering.
        const auto start = sinePosition (chain);
        out = runSine (chain, 2);
        expect (postBypass (chain, 1, false));
        append (out, runSine (chain, 20));
        expect (postBypass (chain, 1, true));
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold, "bypass toggling on a latency slot must not click");

        float worst = 0.0f;
        for (size_t i = 0; i < out.size(); ++i)
            worst = juce::jmax (worst, std::abs (out[i] - sineSample (start + (std::int64_t) i - 480)));

        expectLessThan (worst, kTolerance, "dry and wet stay aligned through the crossfades");
    }

    void testInsertHonoursBypassedFlag()
    {
        beginTest ("testInsertHonoursBypassedFlag");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        auto out = runSine (chain, 2);
        expect (postInsert (chain, 1, std::make_unique<InvertProcessor>(), 0, true));
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold);
        expectLastBlockIs (chain, 1.0f, "a slot inserted with cmd.bypassed stays dry");

        expect (postBypass (chain, 1, false));
        runSine (chain, 20);
        expectLastBlockIs (chain, -1.0f, "un-bypassing it makes it wet");
    }

    void testSeventeenthInsertIsRetired()
    {
        beginTest ("testSeventeenthInsertIsRetired");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        for (int id = 1; id <= ChainProcessor::kMaxSlots; ++id)
            expect (postInsert (chain, id, std::make_unique<GainProcessor> (1.0f), id - 1));

        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16);
        expect (chain.popRetired() == nullptr);

        expect (postInsert (chain, 17, std::make_unique<GainProcessor> (1.0f), 16));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16);

        std::unique_ptr<ChainSlot> retiredSlot (chain.popRetired());
        expect (retiredSlot != nullptr, "the 17th slot is handed straight back");
        if (retiredSlot != nullptr)
            expectEquals (retiredSlot->id, 17);

        expect (chain.popRetired() == nullptr);
        expectLastBlockIs (chain, 1.0f, "16 unity gains pass the input through");
    }

    void testFullRetiredQueueKeepsSlots()
    {
        beginTest ("testFullRetiredQueueKeepsSlots");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        for (int id = 1; id <= ChainProcessor::kMaxSlots; ++id)
            expect (postInsert (chain, id, std::make_unique<GainProcessor> (1.0f), id - 1));
        runSine (chain, 2);

        // Fill the retired queue with inserts into the full chain.
        for (int i = 0; i < ChainProcessor::kRetiredQueueCapacity; ++i)
            expect (postInsert (chain, 100 + i, std::make_unique<GainProcessor> (1.0f)));
        runSine (chain, 1);

        // A remove cannot hand its slot back yet: the slot stays in the chain
        // (dry), but no longer counts as active.
        expect (postRemove (chain, 1));
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 15, "a removing slot is not active");

        // 15 live slots: this insert must go in, not be retired.
        expect (postInsert (chain, 200, std::make_unique<GainProcessor> (1.0f)));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16);

        // 16 live slots: this one must be retired, but the queue is full, so it
        // has to wait, not be lost.
        expect (postInsert (chain, 201, std::make_unique<GainProcessor> (1.0f)));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16);

        auto ids = drainRetired (chain);
        expectEquals ((int) ids.size(), ChainProcessor::kRetiredQueueCapacity);
        std::sort (ids.begin(), ids.end());
        expect (! ids.empty() && ids.front() == 100 && ids.back() == 163, "the first 64 retired slots");

        // Room again: both waiting slots come back on the next blocks.
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 16);

        ids = drainRetired (chain);
        std::sort (ids.begin(), ids.end());
        expect (ids == std::vector<int> { 1, 201 }, "the removed slot and the 17th live insert are both retired");
        expectLastBlockIs (chain, 1.0f, "16 unity gains pass the input through");
    }

    void testReprepareKeepsSlots()
    {
        beginTest ("testReprepareKeepsSlots");

        constexpr double newRate = 44100.0;
        constexpr int newBlock = 512;

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        auto invert = std::make_unique<InvertProcessor>();
        auto* invertRaw = invert.get();   // owned by the chain below
        expect (postInsert (chain, 1, std::move (invert)));
        runSine (chain, 20);
        expectLastBlockIs (chain, -1.0f, "invert settled");

        chain.prepare (newRate, newBlock);

        expectEquals (invertRaw->getSampleRate(), newRate, "slot re-prepared at the new rate");
        expectEquals (invertRaw->getBlockSize(), newBlock, "slot re-prepared at the new block size");

        auto expectOutputIs = [&] (float gain, const juce::String& what)
        {
            const auto start = sinePosition (chain);
            const auto out = runSine (chain, 2, 0.25f, 440.0, newRate, newBlock);

            float worst = 0.0f;
            for (size_t i = 0; i < out.size(); ++i)
                worst = juce::jmax (worst, std::abs (out[i] - gain * sineSample (start + (std::int64_t) i, 0.25f, 440.0, newRate)));

            expectLessThan (worst, kTolerance, what);
        };

        runSine (chain, 4, 0.25f, 440.0, newRate, newBlock);
        expectEquals (chain.getNumActiveSlots(), 1);
        expectOutputIs (-1.0f, "after re-prepare, out == -in at 44.1 kHz / 512");

        // An insert still in the command queue when the device changes is
        // re-prepared too (it was prepared for 48 kHz / 256).
        auto gain = std::make_unique<GainProcessor> (0.5f);
        auto* gainRaw = gain.get();
        expect (postInsert (chain, 2, std::move (gain), 1));

        chain.prepare (newRate, newBlock);
        expectEquals (gainRaw->getSampleRate(), newRate, "queued insert re-prepared at the new rate");
        expectEquals (gainRaw->getBlockSize(), newBlock);

        runSine (chain, 20, 0.25f, 440.0, newRate, newBlock);
        expectEquals (chain.getNumActiveSlots(), 2);
        expectOutputIs (-0.5f, "queued insert is in the chain after re-prepare");
    }

    //==========================================================================
    // Index and order: the message thread's model drops a removed slot at once
    // and numbers later Insert/Move indices without it, while the audio side
    // still holds that slot during its fade-out. All processors below are
    // affine maps that do not commute, so the output shows the chain order.

    // Start [A,B,C]; Remove A; Move(B -> 1). Model: [C,B].
    void testMoveAfterRemoveUsesModelIndex()
    {
        beginTest ("testMoveAfterRemoveUsesModelIndex");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        // A = gain 2 keeps every unfaded switch here >= 0.425 on the test signal.
        expect (postInsert (chain, 1, std::make_unique<GainProcessor> (2.0f), 0));     // A
        expect (postInsert (chain, 2, std::make_unique<GainProcessor> (0.5f), 1));     // B
        expect (postInsert (chain, 3, std::make_unique<OffsetProcessor> (0.6f), 2));   // C
        runSine (chain, 20);
        expectLastBlockMatches (chain, [] (std::int64_t n) { return sineSample (n) + 0.6f; }, "[A,B,C]");

        auto out = runSine (chain, 2);
        expect (postRemove (chain, 1));
        expect (postMove (chain, 2, 1));
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold, "no click");
        expectLastBlockMatches (chain, [] (std::int64_t n) { return 0.5f * (sineSample (n) + 0.6f); },
                                "audio order is the model order [C,B]");
        expectEquals (chain.getNumActiveSlots(), 2);
        expect (drainRetired (chain) == std::vector<int> { 1 });
    }

    // Start [A,B,C]; Remove A; Insert X at 2. Model: [B,C,X].
    void testInsertAfterRemoveUsesModelIndex()
    {
        beginTest ("testInsertAfterRemoveUsesModelIndex");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<GainProcessor> (2.0f), 0));     // A
        expect (postInsert (chain, 2, std::make_unique<InvertProcessor>(), 1));        // B
        expect (postInsert (chain, 3, std::make_unique<GainProcessor> (0.5f), 2));     // C
        runSine (chain, 20);
        expectLastBlockIs (chain, -1.0f, "[A,B,C]");

        auto out = runSine (chain, 2);
        expect (postRemove (chain, 1));
        expect (postInsert (chain, 4, std::make_unique<OffsetProcessor> (0.6f), 2));   // X
        append (out, runSine (chain, 40));

        expectLessThan (maxStep (out), kClickThreshold, "no click");
        expectLastBlockMatches (chain, [] (std::int64_t n) { return -0.5f * sineSample (n) + 0.6f; },
                                "audio order is the model order [B,C,X]");
        expectEquals (chain.getNumActiveSlots(), 3);
        expect (drainRetired (chain) == std::vector<int> { 1 });
    }

    // Start [X,A,B] with A bypassed (so A is dry first); Move(B -> 0); Move(A -> 0).
    // Model: [A,B,X].
    void testMovesApplyInCommandOrder()
    {
        beginTest ("testMovesApplyInCommandOrder");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        expect (postInsert (chain, 1, std::make_unique<OffsetProcessor> (0.6f), 0));          // X
        expect (postInsert (chain, 2, std::make_unique<GainProcessor> (0.5f), 1, true));      // A (bypassed)
        expect (postInsert (chain, 3, std::make_unique<OffsetProcessor> (-0.2f), 2));         // B
        runSine (chain, 20);
        expectLastBlockMatches (chain, [] (std::int64_t n) { return sineSample (n) + 0.4f; }, "[X,A(bypassed),B]");

        auto out = runSine (chain, 2);
        expect (postMove (chain, 3, 0));
        expect (postMove (chain, 2, 0));
        append (out, runSine (chain, 40));

        // A is bypassed, so un-bypass it to make its position audible.
        expect (postBypass (chain, 2, false));
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold, "no click");
        // [A,B,X] = 0.5x - 0.2 + 0.6; the wrong order [B,A,X] would be 0.5x + 0.5.
        expectLastBlockMatches (chain, [] (std::int64_t n) { return 0.5f * sineSample (n) + 0.4f; },
                                "audio order is the model order [A,B,X]");
        expectEquals (chain.getNumActiveSlots(), 3);
    }

    //==========================================================================
    // The 16-slot limit counts live slots only, not slots fading out.
    void testInsertDuringRemoveFadeIsNotRetired()
    {
        beginTest ("testInsertDuringRemoveFadeIsNotRetired");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        for (int id = 1; id <= ChainProcessor::kMaxSlots; ++id)
            expect (postInsert (chain, id, std::make_unique<GainProcessor> (1.0f), id - 1));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16);

        // Insert right after the remove, before its fade-out has ended.
        auto out = runSine (chain, 2);
        expect (postRemove (chain, 1));
        expect (postInsert (chain, 17, std::make_unique<GainProcessor> (1.0f)));
        append (out, runSine (chain, 1));
        expectEquals (chain.getNumActiveSlots(), 16, "the removing slot does not count");
        append (out, runSine (chain, 20));

        expectLessThan (maxStep (out), kClickThreshold);
        expectEquals (chain.getNumActiveSlots(), 16, "the new slot is active");
        expect (drainRetired (chain) == std::vector<int> { 1 }, "only the removed slot is retired, not the new one");
        expectLastBlockIs (chain, 1.0f, "16 unity gains pass the input through");
    }

    // When the physical array (live + fading-out slots) is full, an Insert into
    // a chain with fewer than 16 live slots waits; it is never retired.
    void testPhysicalArrayFullDefersInsert()
    {
        beginTest ("testPhysicalArrayFullDefersInsert");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);

        for (int id = 1; id <= ChainProcessor::kMaxSlots; ++id)
            expect (postInsert (chain, id, std::make_unique<GainProcessor> (1.0f), id - 1));
        runSine (chain, 2);

        // Fill the retired queue, so removed slots stay in the array.
        for (int i = 0; i < ChainProcessor::kRetiredQueueCapacity; ++i)
            expect (postInsert (chain, 100 + i, std::make_unique<GainProcessor> (1.0f)));
        runSine (chain, 1);

        for (int id = 1; id <= ChainProcessor::kMaxSlots; ++id)
            expect (postRemove (chain, id));
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 0);

        for (int id = 17; id <= 32; ++id)
            expect (postInsert (chain, id, std::make_unique<GainProcessor> (1.0f), id - 17));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16, "16 removing + 16 live slots fit");

        expect (postRemove (chain, 17));
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 15);

        // 15 live but 32 physical: the insert must wait.
        expect (postInsert (chain, 33, std::make_unique<GainProcessor> (1.0f)));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 15, "the insert waits while the array is full");

        expectEquals ((int) drainRetired (chain).size(), ChainProcessor::kRetiredQueueCapacity);
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 16, "the waiting insert went in");

        auto ids = drainRetired (chain);
        std::sort (ids.begin(), ids.end());
        std::vector<int> expected;
        for (int id = 1; id <= 17; ++id)
            expected.push_back (id);
        expect (ids == expected, "slots 1..17 are retired, slot 33 is not");
        expectLastBlockIs (chain, 1.0f, "16 unity gains pass the input through");
    }
};

static ChainProcessorTests chainProcessorTests;
