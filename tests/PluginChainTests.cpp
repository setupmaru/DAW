#include "TestProcessors.h"

#include "plugins/PluginChain.h"

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

// Invert processor that counts its destructor calls, to prove that retired
// slots really are deleted (and when).
class CountingProcessor : public InvertProcessor
{
public:
    ~CountingProcessor() override { ++destroyed(); }

    static int& destroyed()
    {
        static int count = 0;
        return count;
    }
};

juce::String utf8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

// Runs `numBlocks` silent stereo blocks through the audio side, which applies
// queued commands and advances every crossfade.
void runBlocks (ChainProcessor& chain, int numBlocks)
{
    juce::AudioBuffer<float> buffer (2, kBlock);

    for (int i = 0; i < numBlocks; ++i)
    {
        buffer.clear();
        chain.process (buffer);
    }
}

// Enough blocks for any slot fade (0.020 s) or output dip to finish.
constexpr int kSettleBlocks = 40;

std::vector<juce::String> namesOf (const PluginChain& model)
{
    std::vector<juce::String> names;
    for (const auto& entry : model.getEntries())
        names.push_back (entry.name);
    return names;
}

// A model wired to a prepared ChainProcessor that counts callbacks.
struct Fixture
{
    Fixture()
    {
        chain.prepare (kSampleRate, kBlock);
        model = std::make_unique<PluginChain> (chain, formats);
        model->setProcessingSpec (kSampleRate, kBlock);
        model->onChanged = [this] { ++changes; };
        model->onError = [this] (const juce::String& message) { errors.add (message); };
    }

    ChainProcessor chain;
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<PluginChain> model;
    int changes = 0;
    juce::StringArray errors;
};
} // namespace

struct PluginChainTests : public juce::UnitTest
{
    PluginChainTests() : juce::UnitTest ("PluginChainTests", "DAW") {}

    void runTest() override
    {
        testAddRemoveMoveUpdatesModel();
        testRejectsProcessorWithoutStereoLayout();
        testRejectsSeventeenthEntry();
        testPostFailureLeavesModelUnchanged();
        testUnknownIdsAreRejected();
        testRetiredSlotsAreDeleted();
        testRetiredSlotStillInModelIsDropped();
        testDestructorDeletesRetiredSlots();
    }

    //==========================================================================
    void testAddRemoveMoveUpdatesModel()
    {
        beginTest ("add/remove/move update the model in order");

        Fixture f;
        auto* invertRaw = new InvertProcessor();
        auto* gainRaw = new GainProcessor (0.5f);

        expect (f.model->addProcessor (std::unique_ptr<juce::AudioProcessor> (invertRaw), "Invert"));
        expect (f.model->addProcessor (std::unique_ptr<juce::AudioProcessor> (gainRaw), "Gain"));
        expect (namesOf (*f.model) == std::vector<juce::String> { "Invert", "Gain" }, "names in add order");
        expectEquals (f.changes, 2);

        const auto& entries = f.model->getEntries();
        expect (entries[0].processor == invertRaw, "entry points at its processor");
        expect (entries[1].processor == gainRaw, "entry points at its processor");
        expect (! entries[0].bypassed && ! entries[1].bypassed, "new entries are not bypassed");
        expect (entries[0].id != entries[1].id, "ids are unique");

        runBlocks (f.chain, kSettleBlocks);
        expectEquals (f.chain.getNumActiveSlots(), 2);

        const int invertId = entries[0].id;
        const int gainId = entries[1].id;

        expect (f.model->move (gainId, 0));
        expect (namesOf (*f.model) == std::vector<juce::String> { "Gain", "Invert" }, "move reorders");
        expectEquals (f.changes, 3);
        runBlocks (f.chain, kSettleBlocks);

        expect (f.model->setBypassed (gainId, true));
        expect (f.model->getEntries()[0].bypassed, "bypass flag stored");
        expectEquals (f.changes, 4);

        expect (f.model->remove (invertId));
        expectEquals ((int) f.model->getEntries().size(), 1);
        expect (namesOf (*f.model) == std::vector<juce::String> { "Gain" }, "remove drops the entry at once");
        expectEquals (f.changes, 5);

        runBlocks (f.chain, kSettleBlocks);
        f.model->pumpRetired();
        expectEquals (f.chain.getNumActiveSlots(), 1);
        expectEquals ((int) f.model->getEntries().size(), 1);
        expect (f.errors.isEmpty(), "no errors on the normal path");
    }

    void testRejectsProcessorWithoutStereoLayout()
    {
        beginTest ("a processor without a stereo layout is refused");

        Fixture f;
        expect (! f.model->addProcessor (std::make_unique<MonoOnlyProcessor>(), "Mono"));
        expectEquals (f.errors.size(), 1);
        expectEquals (f.errors[0], utf8 ("이 플러그인은 스테레오를 지원하지 않아 추가할 수 없습니다: ") + "Mono");
        expect (f.model->getEntries().empty(), "no entry for a refused processor");
        expectEquals (f.changes, 0);

        runBlocks (f.chain, kSettleBlocks);
        expectEquals (f.chain.getNumActiveSlots(), 0);
    }

    void testRejectsSeventeenthEntry()
    {
        beginTest ("the 17th entry is refused with an error");

        Fixture f;
        for (int i = 0; i < ChainProcessor::kMaxSlots; ++i)
            expect (f.model->addProcessor (std::make_unique<GainProcessor> (1.0f), "Gain " + juce::String (i)));

        CountingProcessor::destroyed() = 0;
        expect (! f.model->addProcessor (std::make_unique<CountingProcessor>(), "Extra"));
        expectEquals (f.errors.size(), 1);
        expectEquals ((int) f.model->getEntries().size(), ChainProcessor::kMaxSlots);
        expectEquals (CountingProcessor::destroyed(), 1, "the refused processor is deleted");

        runBlocks (f.chain, kSettleBlocks);
        expectEquals (f.chain.getNumActiveSlots(), ChainProcessor::kMaxSlots);
    }

    void testPostFailureLeavesModelUnchanged()
    {
        beginTest ("a full command queue leaves the model unchanged");

        Fixture f;
        expect (f.model->addProcessor (std::make_unique<InvertProcessor>(), "A"));
        expect (f.model->addProcessor (std::make_unique<GainProcessor> (0.5f), "B"));
        runBlocks (f.chain, 1);   // drain the two inserts

        const int idA = f.model->getEntries()[0].id;
        const int idB = f.model->getEntries()[1].id;

        // No process() call: the queue fills after kCommandQueueCapacity posts.
        bool lastAccepted = false;
        int failures = 0;

        for (int i = 0; i < ChainProcessor::kCommandQueueCapacity + 10; ++i)
        {
            const bool wanted = (i % 2) == 0;
            const int changesBefore = f.changes;

            if (f.model->setBypassed (idA, wanted))
            {
                lastAccepted = wanted;
                expectEquals (f.changes, changesBefore + 1);
            }
            else
            {
                ++failures;
                expectEquals (f.changes, changesBefore, "no change notification on failure");
            }

            expect (f.model->getEntries()[0].bypassed == lastAccepted, "bypass matches the last accepted call");
        }

        expectEquals (failures, 10);

        // Every other kind of edit is refused too, and the model stays as it was.
        CountingProcessor::destroyed() = 0;
        expect (! f.model->addProcessor (std::make_unique<CountingProcessor>(), "C"));
        expectEquals (CountingProcessor::destroyed(), 1, "a refused processor is deleted");
        expect (! f.model->move (idB, 0));
        expect (! f.model->remove (idA));
        expect (namesOf (*f.model) == std::vector<juce::String> { "A", "B" }, "model unchanged");
        expectEquals ((int) f.model->getEntries().size(), 2);

        // Once the audio side catches up, edits work again.
        runBlocks (f.chain, kSettleBlocks);
        expect (f.model->remove (idA));
        runBlocks (f.chain, kSettleBlocks);
        f.model->pumpRetired();
        expectEquals (f.chain.getNumActiveSlots(), 1);
    }

    void testUnknownIdsAreRejected()
    {
        beginTest ("unknown ids are rejected without posting");

        Fixture f;
        expect (f.model->addProcessor (std::make_unique<InvertProcessor>(), "A"));
        const int changesBefore = f.changes;

        expect (! f.model->remove (12345));
        expect (! f.model->move (12345, 0));
        expect (! f.model->setBypassed (12345, true));
        expectEquals (f.changes, changesBefore);

        // move clamps the destination into the chain.
        expect (f.model->addProcessor (std::make_unique<GainProcessor> (0.5f), "B"));
        expect (f.model->move (f.model->getEntries()[0].id, 99));
        expect (namesOf (*f.model) == std::vector<juce::String> { "B", "A" }, "move clamps to the end");
    }

    void testRetiredSlotsAreDeleted()
    {
        beginTest ("retired slots are deleted by pumpRetired");

        Fixture f;
        CountingProcessor::destroyed() = 0;

        expect (f.model->addProcessor (std::make_unique<CountingProcessor>(), "A"));
        expect (f.model->addProcessor (std::make_unique<CountingProcessor>(), "B"));
        runBlocks (f.chain, kSettleBlocks);

        expect (f.model->remove (f.model->getEntries()[0].id));
        runBlocks (f.chain, kSettleBlocks);
        expectEquals (CountingProcessor::destroyed(), 0, "not deleted before the pump");

        f.model->pumpRetired();
        expectEquals (CountingProcessor::destroyed(), 1, "the removed processor is deleted");
        expect (f.chain.popRetired() == nullptr, "retired queue is empty");
        expect (namesOf (*f.model) == std::vector<juce::String> { "B" });
        expect (f.errors.isEmpty(), "a normal removal is not an error");
    }

    void testRetiredSlotStillInModelIsDropped()
    {
        beginTest ("a retired slot still in the model is dropped with an error");

        Fixture f;

        // Fill the audio side behind the model's back.
        for (int i = 0; i < ChainProcessor::kMaxSlots; ++i)
        {
            auto slot = makeSlot (1000 + i, std::make_unique<GainProcessor> (1.0f), kSampleRate, kBlock);
            ChainCommand cmd { ChainCommandType::Insert };
            cmd.slotId = slot->id;
            cmd.index = i;
            cmd.slot = slot.get();
            expect (f.chain.post (cmd));
            slot.release();
        }

        runBlocks (f.chain, kSettleBlocks);
        expectEquals (f.chain.getNumActiveSlots(), ChainProcessor::kMaxSlots);

        CountingProcessor::destroyed() = 0;
        expect (f.model->addProcessor (std::make_unique<CountingProcessor>(), "Ghost"));
        expectEquals ((int) f.model->getEntries().size(), 1);
        const int changesBefore = f.changes;

        runBlocks (f.chain, kSettleBlocks);   // the audio side hands the insert back
        f.model->pumpRetired();

        expect (f.model->getEntries().empty(), "the entry is dropped");
        expectEquals (f.errors.size(), 1);
        expect (f.errors[0].contains ("Ghost"), "the error names the plugin");
        expectEquals (f.changes, changesBefore + 1, "the model change is announced");
        expectEquals (CountingProcessor::destroyed(), 1, "the slot is deleted");
        expect (f.chain.popRetired() == nullptr);
    }

    void testDestructorDeletesRetiredSlots()
    {
        beginTest ("destroying the model deletes slots waiting in the retired queue");

        ChainProcessor chain;
        chain.prepare (kSampleRate, kBlock);
        juce::AudioPluginFormatManager formats;
        CountingProcessor::destroyed() = 0;

        {
            PluginChain model (chain, formats);
            model.setProcessingSpec (kSampleRate, kBlock);
            expect (model.addProcessor (std::make_unique<CountingProcessor>(), "A"));
            expect (model.addProcessor (std::make_unique<CountingProcessor>(), "B"));
            runBlocks (chain, kSettleBlocks);
            expect (model.remove (model.getEntries()[0].id));
            runBlocks (chain, kSettleBlocks);   // A is now in the retired queue
            expectEquals (CountingProcessor::destroyed(), 0);
        }

        expectEquals (CountingProcessor::destroyed(), 1, "the model's destructor deleted the retired slot");
        expect (chain.popRetired() == nullptr);
        expectEquals (chain.getNumActiveSlots(), 1, "the live slot stays with the ChainProcessor");
    }
};

static PluginChainTests pluginChainTests;
