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

        // A remove cannot hand its slot back yet: the slot must stay in the chain.
        expect (postRemove (chain, 1));
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 16, "remove waits while the retired queue is full");

        // Neither can an insert into the full chain: it must wait, not be lost.
        expect (postInsert (chain, 200, std::make_unique<GainProcessor> (1.0f)));
        runSine (chain, 2);
        expectEquals (chain.getNumActiveSlots(), 16);

        auto ids = drainRetired (chain);
        expectEquals ((int) ids.size(), ChainProcessor::kRetiredQueueCapacity);
        std::sort (ids.begin(), ids.end());
        expect (! ids.empty() && ids.front() == 100 && ids.back() == 163, "the first 64 retired slots");

        // Room again: both waiting slots come back on the next blocks.
        runSine (chain, 20);
        expectEquals (chain.getNumActiveSlots(), 15, "the remove completed");

        ids = drainRetired (chain);
        std::sort (ids.begin(), ids.end());
        expect (ids == std::vector<int> { 1, 200 }, "the removed slot and the 17th insert are both retired");
        expectLastBlockIs (chain, 1.0f, "the remaining 15 unity gains pass the input through");
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
};

static ChainProcessorTests chainProcessorTests;
