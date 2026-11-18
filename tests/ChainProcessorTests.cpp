#include "TestProcessors.h"

#include <cmath>

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
    }

    //==========================================================================
    bool postInsert (ChainProcessor& chain, int id, std::unique_ptr<juce::AudioProcessor> processor, int index = 0)
    {
        auto slot = makeSlot (id, std::move (processor), kSampleRate, kBlock);
        ChainCommand cmd { ChainCommandType::Insert };
        cmd.slotId = id;
        cmd.index = index;
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

    // Runs one block and checks out == gain * in for every sample of it.
    void expectLastBlockIs (ChainProcessor& chain, float gain, const juce::String& what)
    {
        const auto start = sinePosition (chain);
        const auto out = runSine (chain, 1);

        float worst = 0.0f;
        for (size_t i = 0; i < out.size(); ++i)
            worst = juce::jmax (worst, std::abs (out[i] - gain * sineSample (start + (std::int64_t) i)));

        expectLessThan (worst, kTolerance, what);
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
};

static ChainProcessorTests chainProcessorTests;
