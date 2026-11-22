#pragma once

#include "Session.h"

#include <atomic>
#include <functional>

// Renders [start, end) of the session's mix offline (faster than real time)
// into a stereo 24-bit WAV file, through every track's inserts, faders and the
// master bus, compensating the mix latency so the file lines up with the
// timeline. The device is detached while it runs.
//
// Threads: create, start and destroy on the message thread; the rendering runs
// on the bouncer's own thread. onDone is called on the message thread with ""
// or a Korean error. The UI must block every edit until then.
class Bouncer : private juce::Thread
{
public:
    Bouncer (Session&, juce::File target, juce::int64 start, juce::int64 end);
    ~Bouncer() override;   // cancels a running render (the file is then deleted)

    void start (std::function<void (const juce::String& error)> onDone);
    float getProgress() const noexcept { return progress.load(); }
    void cancel() { signalThreadShouldExit(); }

    // The render itself, on the calling thread (tests). Returns "" or an error.
    juce::String renderNow();

private:
    void run() override;
    juce::String prepare();
    void finish (const juce::String& error);

    Session& session;
    MixEngine& engine;
    const juce::File target;
    const juce::int64 rangeStart, rangeEnd;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::atomic<float> progress { 0.0f };
    std::function<void (const juce::String&)> onDone;
    bool engineOffline = false;
    int latency = 0;   // mix latency, measured on the message thread in prepare()

    JUCE_DECLARE_WEAK_REFERENCEABLE (Bouncer)
};
