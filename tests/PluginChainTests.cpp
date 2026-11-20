#include "TestProcessors.h"

#include "plugins/PluginChain.h"
#include "audio/AudioEngine.h"

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

// out = in + offset. Does not commute with a gain, so it shows the chain order.
class OffsetProcessor : public TestProcessorBase
{
public:
    explicit OffsetProcessor (float o) : offset (o) {}

    using TestProcessorBase::processBlock;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::add (buffer.getWritePointer (ch), offset, buffer.getNumSamples());
    }

private:
    float offset;
};

// Simulates a device restart between reading the spec and posting an Insert.
class RestartDuringPrepareProcessor : public GainProcessor
{
public:
    explicit RestartDuringPrepareProcessor (ChainProcessor& c) : GainProcessor (0.5f), chain (c) {}

    void prepareToPlay (double, int) override
    {
        if (! restarted)
        {
            restarted = true;
            chain.prepare (44100.0, 128);
        }
    }

private:
    ChainProcessor& chain;
    bool restarted = false;
};

// Runs one block of constant `input` (both channels) and returns the left
// channel's last output sample.
float runConstant (ChainProcessor& chain, float input)
{
    juce::AudioBuffer<float> buffer (2, kBlock);
    buffer.clear();
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), input, kBlock);

    chain.process (buffer);
    return buffer.getSample (0, kBlock - 1);
}

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
        testMoveChangesAudioOrder();
        testAddPluginFailureReportsError();
        testAddPluginCompletingAfterDestructionIsIgnored();
        testRejectsProcessorWithoutStereoLayout();
        testRejectsSeventeenthEntry();
        testPostFailureLeavesModelUnchanged();
        testUnknownIdsAreRejected();
        testRetiredSlotsAreDeleted();
        testRetiredSlotStillInModelIsDropped();
        testDestructorDeletesRetiredSlots();
        testEditsWithoutDevice();
        testStoppedRemovalsPreserveLifetime();
        testNewSlotUsesEngineSpec();
        testRestartDuringSlotPreparation();
    }

    //==========================================================================
    void testEditsWithoutDevice()
    {
        beginTest ("edits without a device do not fill the command queue and survive startup");

        AudioEngine engine;
        juce::AudioPluginFormatManager formats;
        PluginChain model (engine.getChain(), formats);
        juce::StringArray errors;
        model.onError = [&] (const juce::String& message) { errors.add (message); };
        expect (model.addProcessor (std::make_unique<GainProcessor> (0.5f), "Gain"));
        expect (model.addProcessor (std::make_unique<OffsetProcessor> (0.25f), "Offset"));
        const int gainId = model.getEntries()[0].id;
        int accepted = 0;
        for (int i = 0; i < 256; ++i)
            if (model.setBypassed (gainId, i % 2 == 0))
                ++accepted;
        expectEquals (accepted, 256);
        expect (model.move (model.getEntries()[1].id, 0));
        expectEquals (engine.getChain().getNumActiveSlots(), 2);
        expect (errors.isEmpty(), "no queue-full error without an audio callback");

        engine.prepareForTest (44100.0, 128);
        juce::AudioBuffer<float> input (1, kBlock), output (2, kBlock);
        juce::FloatVectorOperations::fill (input.getWritePointer (0), 1.0f, kBlock);
        engine.audioDeviceIOCallbackWithContext (input.getArrayOfReadPointers(), 1,
                                                 output.getArrayOfWritePointers(), 2, kBlock, {});
        for (int ch = 0; ch < 2; ++ch)
            expectWithinAbsoluteError (output.getSample (ch, kBlock - 1), 0.625f, 1.0e-5f,
                                       "startup preserves order and the last bypass state");
    }

    void testStoppedRemovalsPreserveLifetime()
    {
        beginTest ("stopped edits exceed both queue capacities without deleting processors before onChanged");

        AudioEngine engine;
        engine.prepareForTest (kSampleRate, kBlock);
        juce::AudioPluginFormatManager formats;
        PluginChain model (engine.getChain(), formats);
        CountingProcessor::destroyed() = 0;
        expect (model.addProcessor (std::make_unique<CountingProcessor>(), "Queued"));
        // Stop with the Insert still queued, before any audio block has run.
        engine.audioDeviceStopped();
        expectEquals (engine.getChain().getNumActiveSlots(), 1);

        bool aliveDuringChange = true;
        model.onChanged = [&] { aliveDuringChange = aliveDuringChange && CountingProcessor::destroyed() == 0; };
        int removed = 0;
        for (int i = 0; i < 128; ++i)
        {
            if (model.getEntries().empty())
                expect (model.addProcessor (std::make_unique<CountingProcessor>(), "Stopped"));
            if (! model.getEntries().empty() && model.remove (model.getEntries()[0].id))
                ++removed;
        }
        expectEquals (removed, 128);
        expect (aliveDuringChange, "editors can close before processor destruction");
        expectEquals (CountingProcessor::destroyed(), 0, "only the message-thread retirement pump deletes slots");
        expectEquals (engine.getChain().getNumActiveSlots(), 0);
        model.pumpRetired();
        expectEquals (CountingProcessor::destroyed(), 128);
        expect (model.getEntries().empty());
    }

    void testNewSlotUsesEngineSpec()
    {
        beginTest ("new slots use the engine spec before the UI receives its change notification");
        Fixture f;
        f.chain.prepare (44100.0, 128);
        auto processor = std::make_unique<GainProcessor> (0.5f);
        auto* raw = processor.get();
        expect (f.model->addProcessor (std::move (processor), "Gain"));
        expectEquals (raw->getSampleRate(), 44100.0);
        expectEquals (raw->getBlockSize(), 128);
    }

    void testRestartDuringSlotPreparation()
    {
        beginTest ("an Insert is re-prepared if the device restarts during slot creation");
        Fixture f;
        auto processor = std::make_unique<RestartDuringPrepareProcessor> (f.chain);
        auto* raw = processor.get();
        expect (f.model->addProcessor (std::move (processor), "Restart"));
        expectEquals (raw->getSampleRate(), 44100.0);
        expectEquals (raw->getBlockSize(), 128);
        runBlocks (f.chain, kSettleBlocks);
        expectWithinAbsoluteError (runConstant (f.chain, 1.0f), 0.5f, 1.0e-5f);
    }

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

    void testMoveChangesAudioOrder()
    {
        beginTest ("the audio order follows the model after move");

        Fixture f;
        expect (f.model->addProcessor (std::make_unique<GainProcessor> (0.5f), "Gain"));
        expect (f.model->addProcessor (std::make_unique<OffsetProcessor> (0.25f), "Offset"));
        runBlocks (f.chain, kSettleBlocks);

        // Gain then Offset: 0.5 * 1 + 0.25
        expectWithinAbsoluteError (runConstant (f.chain, 1.0f), 0.75f, 1.0e-5f, "Gain -> Offset");

        expect (f.model->move (f.model->getEntries()[1].id, 0));
        expect (namesOf (*f.model) == std::vector<juce::String> { "Offset", "Gain" });

        for (int i = 0; i < kSettleBlocks; ++i)
            runConstant (f.chain, 1.0f);

        // Offset then Gain: 0.5 * (1 + 0.25)
        expectWithinAbsoluteError (runConstant (f.chain, 1.0f), 0.625f, 1.0e-5f, "Offset -> Gain");
    }

    static juce::PluginDescription unknownPlugin()
    {
        juce::PluginDescription desc;
        desc.name = "Missing";
        desc.pluginFormatName = "NoSuchFormat";
        desc.fileOrIdentifier = "/nowhere/Missing.vst3";
        return desc;
    }

    void testAddPluginFailureReportsError()
    {
        beginTest ("addPlugin reports a creation failure and leaves the model unchanged");

        Fixture f;
        f.model->addPlugin (unknownPlugin());
        expect (f.errors.isEmpty(), "the failure is reported asynchronously");

        expect (pumpMessagesUntil ([&f] { return ! f.errors.isEmpty(); }), "onError is called");
        expectEquals (f.errors.size(), 1);
        expect (f.errors[0].startsWith (utf8 ("플러그인을 불러오지 못했습니다: ") + "Missing"), f.errors[0]);
        expect (f.model->getEntries().empty(), "no entry");
        expectEquals (f.changes, 0);

        runBlocks (f.chain, kSettleBlocks);
        expectEquals (f.chain.getNumActiveSlots(), 0);
    }

    void testAddPluginCompletingAfterDestructionIsIgnored()
    {
        beginTest ("addPlugin completing after the model is destroyed is ignored");

        Fixture f;
        bool called = false;
        f.model->onError = [&called] (const juce::String&) { called = true; };
        f.model->addPlugin (unknownPlugin());
        f.model.reset();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        expect (! called, "no callback into a destroyed model");
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

        // The UI closes editors in onChanged, so it must run while the
        // processor still exists.
        int destroyedWhenChanged = -1;
        f.model->onChanged = [&f, &destroyedWhenChanged]
        {
            ++f.changes;
            destroyedWhenChanged = CountingProcessor::destroyed();
        };

        runBlocks (f.chain, kSettleBlocks);   // the audio side hands the insert back
        f.model->pumpRetired();
        expectEquals (destroyedWhenChanged, 0, "onChanged runs before the processor is deleted");

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
