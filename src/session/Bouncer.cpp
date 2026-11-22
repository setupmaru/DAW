#include "Bouncer.h"

#include <algorithm>

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }
constexpr int kBlock = 1024;
} // namespace

Bouncer::Bouncer (Session& s, juce::File file, juce::int64 from, juce::int64 to)
    : juce::Thread ("DAW bounce"), session (s), engine (s.getEngine()), target (std::move (file)),
      rangeStart (from), rangeEnd (to)
{
}

Bouncer::~Bouncer()
{
    stopThread (10000);

    if (writer != nullptr)
    {
        writer.reset();
        target.deleteFile();   // unfinished
    }

    if (engineOffline)
        engine.endOffline();
}

juce::String Bouncer::prepare()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (rangeEnd <= rangeStart)
        return utf8 ("내보낼 구간이 비어 있습니다.");

    target.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
        return utf8 ("파일을 만들 수 없습니다: ") + target.getFullPathName();

    juce::WavAudioFormat wav;
    writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                              .withSampleRate (session.getSampleRate())
                                              .withNumChannels (2)
                                              .withBitsPerSample (24));
    if (writer == nullptr)
        return utf8 ("파일을 만들 수 없습니다: ") + target.getFullPathName();

    session.publish();
    engine.beginOffline (session.getSampleRate(), kBlock);
    engineOffline = true;

    // Latency after preparing (plugins may report it in prepareToPlay), read
    // here so the render thread never touches the session's track list.
    latency = engine.getMaster().getChain().getTotalLatencySamples();
    int maxTrack = 0;
    for (const auto& track : session.getTracks())
        maxTrack = std::max (maxTrack, track->getAudio().getChain().getTotalLatencySamples());
    latency += std::min (maxTrack, TrackAudio::kMaxCompensation);
    return {};
}

juce::String Bouncer::renderNow()
{
    if (writer == nullptr)
        if (auto error = prepare(); error.isNotEmpty())
            return error;

    juce::AudioBuffer<float> buffer (2, kBlock);
    const auto total = (rangeEnd - rangeStart) + latency;
    juce::int64 rendered = 0;
    juce::int64 toSkip = latency;

    // The first blocks settle delay compensation (which dips the mix while
    // delays change): render a pre-roll before `start` and throw it away.
    const auto preRoll = (juce::int64) (session.getSampleRate() * 0.1) + latency;
    for (auto pos = rangeStart - preRoll; pos < rangeStart; pos += kBlock)
    {
        const int n = (int) std::min<juce::int64> (kBlock, rangeStart - pos);
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, n);
        engine.renderOffline (pos, view);
    }

    while (rendered < total)
    {
        if (threadShouldExit())
            return utf8 ("내보내기를 취소했습니다.");

        const int n = (int) std::min<juce::int64> (kBlock, total - rendered);
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, n);
        engine.renderOffline (rangeStart + rendered, view);
        rendered += n;

        int offset = 0;
        if (toSkip > 0)
        {
            offset = (int) std::min<juce::int64> (toSkip, n);
            toSkip -= offset;
        }

        if (offset < n)
        {
            const float* channels[2] = { buffer.getReadPointer (0, offset), buffer.getReadPointer (1, offset) };
            if (! writer->writeFromFloatArrays (channels, 2, n - offset))
                return utf8 ("파일에 쓰는 중 오류가 났습니다: ") + target.getFullPathName();
        }

        progress.store ((float) rendered / (float) total);
    }

    writer.reset();   // finishes the file
    return {};
}

void Bouncer::start (std::function<void (const juce::String&)> callback)
{
    JUCE_ASSERT_MESSAGE_THREAD
    onDone = std::move (callback);

    if (auto error = prepare(); error.isNotEmpty())
    {
        finish (error);
        return;
    }

    startThread();
}

void Bouncer::run()
{
    const auto error = renderNow();

    juce::MessageManager::callAsync ([weak = juce::WeakReference<Bouncer> (this), error]
    {
        if (auto* self = weak.get())
            self->finish (error);
    });
}

void Bouncer::finish (const juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopThread (10000);

    if (writer != nullptr)
    {
        writer.reset();
        target.deleteFile();
    }

    if (engineOffline)
    {
        engine.endOffline();
        engineOffline = false;
    }

    if (auto callback = onDone)
        callback (error);   // may delete this object
}
