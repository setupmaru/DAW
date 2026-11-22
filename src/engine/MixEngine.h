#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Clip.h"
#include "TrackAudio.h"
#include "audio/LockFreeQueue.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

//==============================================================================
// What the audio thread plays: every track's clips, flattened into raw
// pointers. Built on the message thread from the session, handed over with
// MixEngine::publish(), and deleted on the message thread once the audio
// thread has moved on to a newer one. The keep* members hold the shared
// objects alive for as long as the snapshot exists.
struct ClipPlayback
{
    const float* channels[2] {};
    int numChannels = 0;
    juce::int64 start = 0, length = 0, offset = 0, fadeIn = 0, fadeOut = 0;
    float gain = 1.0f;
};

struct TrackPlayback
{
    TrackAudio* audio = nullptr;
    std::vector<ClipPlayback> clips;   // sorted by start, not overlapping
};

struct PlaybackSnapshot
{
    std::vector<TrackPlayback> tracks;
    std::vector<std::shared_ptr<TrackAudio>> keepTracks;
    std::vector<AudioDataPtr> keepData;

    void addTrack (std::shared_ptr<TrackAudio>, const std::vector<Clip>&);
};

//==============================================================================
// One recording pass. Made on the message thread (MixEngine::startRecording),
// written by the audio thread, handed back through popFinishedRecording().
struct RecordTake
{
    int trackId = 0;
    juce::File file;
    int firstChannel = 0, numChannels = 1;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::int64 samplesWritten = 0;   // audio thread while recording
    bool overflowed = false;          // the disk could not keep up
};

struct RecordJob
{
    std::vector<RecordTake> takes;
    juce::int64 startPosition = 0;    // timeline position of the first written sample (audio thread)
    int latencySamples = 0;           // round trip at the start: the first samples to drop
};

//==============================================================================
// The DAW's audio engine. Opens the device and, every block:
//
//   for each track: clips (or, when armed, the live input) -> inserts
//                   -> delay compensation -> fader / pan / mute / solo  ─┐
//   master bus:  sum ─> inserts -> master fader -> (click) -> outputs 1-2
//
// Recording writes the raw input of every armed track to a WAV file on a
// background thread while playing. Loop playback wraps between two positions.
//
// Threads: everything public is message-thread only unless noted. The device
// callback never allocates, frees, locks or logs. Snapshots and record jobs
// are handed over with atomics and lock-free queues; whatever the audio thread
// is done with comes back to the message thread to be freed (collectGarbage(),
// popFinishedRecording()).
class MixEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int kMaxInputChannels = 16;
    static constexpr double kPdcDipSeconds = 0.010;

    MixEngine();
    ~MixEngine() override;

    // Opens the device described by savedDeviceState (createStateXml() of an
    // earlier run), falling back to the default device. Returns "" on success,
    // otherwise a Korean error. The engine stays registered either way.
    juce::String initialise (const juce::XmlElement* savedDeviceState = nullptr);
    void shutdown();
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }

    // The rate and block size the engine is prepared for (0 when no device).
    double getSampleRate() const noexcept { return preparedRate.load(); }
    int getBlockSize() const noexcept { return preparedBlock.load(); }

    //--------------------------------------------------------------------------
    // Tracks. The returned object is kept prepared for the device; its last
    // reference must be dropped on the message thread, before this engine dies.
    std::shared_ptr<TrackAudio> createTrackAudio();
    TrackAudio& getMaster() noexcept { return *master; }

    // Replaces what is played. Any thread may build the snapshot; publish on
    // the message thread.
    void publish (std::unique_ptr<PlaybackSnapshot>);
    void collectGarbage();   // deletes snapshots the audio thread has finished with

    // Largest track insert latency plus the master insert latency (samples).
    int getMixLatency() const noexcept { return mixLatency.load(); }

    //--------------------------------------------------------------------------
    // Transport.
    bool play();   // false when no device is running
    void stop();
    bool isPlaying() const noexcept { return playing.load(); }
    void locate (juce::int64 position);
    juce::int64 getPosition() const noexcept { return position.load(); }

    void setLoop (bool enabled, juce::int64 start, juce::int64 end);
    bool isLoopEnabled() const noexcept { return loopEnabled.load(); }

    // Metronome click on the outputs (not recorded, not bounced). 4/4.
    void setClickEnabled (bool e) noexcept { clickEnabled.store (e); }
    bool isClickEnabled() const noexcept { return clickEnabled.load(); }
    void setTempo (double bpm) noexcept { tempo.store (juce::jlimit (20.0, 400.0, bpm)); }

    // Starts recording `job` (its takes' writers ready) at the current
    // position and starts playback. Loop playback is off while recording.
    // False when the device is not running or a recording is already active.
    bool startRecording (std::unique_ptr<RecordJob>);
    bool isRecording() const noexcept { return recording.load(); }
    // A job the audio thread has finished (after stop()), or nullptr.
    // The caller flushes it by deleting the writers.
    std::unique_ptr<RecordJob> popFinishedRecording();
    // ThreadedWriter::write() wakes this thread with WaitableEvent::signal(),
    // a short lock per take and block: JUCE's standard recording path, the
    // one accepted exception to "the callback never locks".
    juce::TimeSliceThread& getWriterThread() noexcept { return writerThread; }

    // The device's round-trip latency plus the mix latency: how many samples
    // a recording made now is late. Message thread.
    int getRecordingLatency();

    //--------------------------------------------------------------------------
    // Offline rendering (bounce). beginOffline() detaches the device and
    // prepares everything for `blockSize`; renderOffline() may then run on any
    // one thread, repeatedly; endOffline() reattaches the device. The UI must
    // not edit plugin chains in between.
    void beginOffline (double sampleRate, int blockSize);
    // Renders out.getNumSamples() (<= blockSize) samples of the mix at `pos`
    // into a stereo buffer. No input, no click.
    void renderOffline (juce::int64 pos, juce::AudioBuffer<float>& out);
    void endOffline();
    bool isOffline() const noexcept { return offline.load(); }

    //--------------------------------------------------------------------------
    // Tests: prepare without a device (like the device starting), then call
    // audioDeviceIOCallbackWithContext directly.
    void prepareForTest (double sampleRate, int blockSize);

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;

private:
    struct Registry;   // every live TrackAudio, for prepare / stop

    void prepareAll (double sampleRate, int blockSize);
    void stopAll();
    void takeSnapshot() noexcept;
    void finishRecording() noexcept;
    void renderChunk (juce::int64 pos, int n, bool isPlayingNow, bool isRecordingNow,
                      const float* const* inputs, int numInputs, int inputOffset, bool withClick) noexcept;
    void updateCompensation() noexcept;
    void writeTakes (const float* const* inputs, int numInputs, int inputOffset, int n) noexcept;
    void addClick (juce::int64 pos, int n) noexcept;

    juce::AudioDeviceManager deviceManager;
    juce::TimeSliceThread writerThread { "DAW recorder" };

    std::shared_ptr<Registry> registry;
    std::shared_ptr<TrackAudio> master;

    // Snapshot handover. The message thread swaps new ones into `pending`;
    // the audio thread takes it, and retires the one it replaces.
    std::atomic<PlaybackSnapshot*> pending { nullptr };
    PlaybackSnapshot* current = nullptr;                 // audio thread
    LockFreeQueue<PlaybackSnapshot*, 64> retired;

    // Transport. `position` is written by the audio thread only.
    std::atomic<bool> playing { false }, playRequested { false };
    std::atomic<juce::int64> position { 0 }, locateTarget { -1 };
    std::atomic<bool> loopEnabled { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };
    std::atomic<bool> clickEnabled { false };
    std::atomic<double> tempo { 120.0 };

    // Recording.
    std::atomic<RecordJob*> pendingRecord { nullptr };
    RecordJob* activeRecord = nullptr;                   // audio thread
    std::atomic<bool> recording { false };
    LockFreeQueue<RecordJob*, 8> finishedRecords;
    juce::AudioBuffer<float> silence;                    // for takes on missing inputs
    std::vector<std::unique_ptr<RecordJob>> reclaimedRecords;   // message thread: never started

    // Delay compensation: changes wait for the master output to dip to 0.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> pdcGain { 1.0f };
    int pdcHoldSamples = 0;
    std::atomic<int> mixLatency { 0 };

    std::atomic<double> preparedRate { 0.0 };
    std::atomic<int> preparedBlock { 0 };
    std::atomic<bool> deviceRunning { false };
    std::atomic<bool> offline { false };
    bool callbackAttached = false;

    JUCE_DECLARE_NON_COPYABLE (MixEngine)
};
