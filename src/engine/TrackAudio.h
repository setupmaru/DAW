#pragma once

#include "audio/ChainProcessor.h"
#include "audio/LevelMeterSource.h"

#include <atomic>

// The audio half of one track (or of the master bus): its insert chain, its
// fader, pan, mute/solo/arm state, its input routing and its meters.
//
// Ownership: through std::shared_ptr, by the session's Track and by every
// playback snapshot that lists it, so it lives as long as the audio thread may
// use it. The last reference is always dropped on the message thread (MixEngine
// deletes retired snapshots there). Created only by MixEngine::createTrackAudio(),
// which keeps it prepared for the open device.
//
// Threads: the setters/getters may be used from any thread; the audio thread
// reads them once per block and ramps every gain change over kGainRampSeconds,
// so fader moves, pan, mute and solo never click. prepare() runs with IO
// stopped. The process* functions are audio-thread only and never allocate,
// lock or free.
class TrackAudio
{
public:
    static constexpr double kGainRampSeconds = 0.010;
    static constexpr int kMaxCompensation = 1 << 15;   // plugin delay compensation limit (samples)
    static constexpr float kMinDb = -100.0f;           // at or below: silence

    TrackAudio() { chain.setProcessingActive (false); }

    ChainProcessor& getChain() noexcept { return chain; }

    void setVolumeDb (float db) noexcept   { volumeDb.store (db); }
    float getVolumeDb() const noexcept     { return volumeDb.load(); }
    void setPan (float p) noexcept         { pan.store (juce::jlimit (-1.0f, 1.0f, p)); }
    float getPan() const noexcept          { return pan.load(); }
    void setMute (bool m) noexcept         { mute.store (m); }
    bool isMuted() const noexcept          { return mute.load(); }
    void setSolo (bool s) noexcept         { solo.store (s); }
    bool isSoloed() const noexcept         { return solo.load(); }
    void setArmed (bool a) noexcept        { armed.store (a); }
    bool isArmed() const noexcept          { return armed.load(); }
    // Input monitoring ("I"): the track always plays its input instead of
    // its clips, armed or not, playing or not.
    void setInputMonitor (bool m) noexcept { inputMonitor.store (m); }
    bool isInputMonitor() const noexcept   { return inputMonitor.load(); }

    // Input for monitoring and recording: device input `firstChannel`, or the
    // pair (firstChannel, firstChannel + 1) when stereo.
    void setInput (int firstChannel, bool stereo) noexcept { inputChannel.store (firstChannel); inputStereo.store (stereo); }
    int getInputChannel() const noexcept   { return inputChannel.load(); }
    bool isInputStereo() const noexcept    { return inputStereo.load(); }

    LevelMeterSource meterL, meterR;      // post-fader peaks

    //==========================================================================
    // Engine side.

    // IO stopped. Allocates every buffer the process functions use.
    void prepare (double sampleRate, int maxBlockSize);

    // The track's working buffer (2 channels, at least maxBlockSize samples).
    float* const* getBuffer() noexcept { return buffer.getArrayOfWritePointers(); }

    // Runs the insert chain over the first n samples of the buffer, then
    // delays them by the compensation set with setCompensation().
    void processInserts (int n) noexcept;

    // Applies fader, pan and (solo-)mute, adds the result into dest[0..1]
    // and feeds the meters. `silenced` is mute or another track's solo.
    void mixInto (float* const* dest, int n, bool silenced) noexcept;

    // Applies the fader (and mute) in place, without pan. For the master bus.
    void applyFaderInPlace (int n) noexcept;

    int getCompensation() const noexcept   { return compensation; }
    void setCompensation (int samples) noexcept { compensation = juce::jlimit (0, kMaxCompensation, samples); }

private:
    float targetGain (bool silenced) const noexcept;

    ChainProcessor chain;

    std::atomic<float> volumeDb { 0.0f }, pan { 0.0f };
    std::atomic<bool> mute { false }, solo { false }, armed { false }, inputMonitor { false }, inputStereo { false };
    std::atomic<int> inputChannel { 0 };

    juce::AudioBuffer<float> buffer;
    juce::AudioBuffer<float> delayBuffer;   // compensation ring buffer, 2 channels
    int delayWrite = 0;
    int compensation = 0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainL { 1.0f }, gainR { 1.0f };

    JUCE_DECLARE_NON_COPYABLE (TrackAudio)
};
