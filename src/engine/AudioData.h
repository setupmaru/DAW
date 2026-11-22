#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <utility>
#include <vector>

// The audio of one file, decoded into memory at the session's sample rate,
// plus a min/max overview for drawing waveforms.
//
// Immutable once made, so clips, playback snapshots and the UI share it through
// std::shared_ptr<const AudioData> without locks. The audio thread only reads
// `samples`; the last reference is always dropped on the message thread.
struct AudioData
{
    static constexpr int kPeakBlock = 256;   // samples per overview entry

    // Where the audio lives on disk (may be empty in tests). The one mutable
    // field: the message thread updates it when a save copies the file into
    // the session folder. The audio thread never reads it.
    mutable juce::File file;
    juce::AudioBuffer<float> samples;    // 1 or 2 channels
    double sampleRate = 0.0;             // the session rate it was converted to
    std::vector<std::pair<float, float>> peaks;   // min/max over all channels per kPeakBlock

    juce::int64 getLength() const noexcept { return samples.getNumSamples(); }
    int getNumChannels() const noexcept    { return samples.getNumChannels(); }

    // min/max over [start, end), clamped to the data. Uses the overview when
    // the range covers whole blocks, the samples otherwise.
    std::pair<float, float> getRange (juce::int64 start, juce::int64 end) const noexcept;

    void buildPeaks();
};

using AudioDataPtr = std::shared_ptr<const AudioData>;

// Builds AudioData from samples already in memory (recordings, tests).
AudioDataPtr makeAudioData (juce::AudioBuffer<float>&& samples, double sampleRate, const juce::File& file = {});

// Decodes a whole file (WAV, AIFF, FLAC, Ogg, MP3 where the OS supports it)
// and converts it to `targetSampleRate`. Keeps the first two channels.
// Returns nullptr and a Korean message in `error` on failure.
AudioDataPtr loadAudioFile (juce::AudioFormatManager&, const juce::File&, double targetSampleRate, juce::String& error);
