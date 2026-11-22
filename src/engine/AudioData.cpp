#include "AudioData.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

// About 3 hours of stereo at 192 kHz would still fit; the int sample index of
// juce::AudioBuffer is the real limit.
constexpr juce::int64 kMaxSamples = std::numeric_limits<int>::max() - 16;
} // namespace

std::pair<float, float> AudioData::getRange (juce::int64 start, juce::int64 end) const noexcept
{
    start = std::max<juce::int64> (0, start);
    end = std::min (end, getLength());

    float lo = 0.0f, hi = 0.0f;
    bool any = false;

    auto addSample = [&] (float v)
    {
        if (! any) { lo = hi = v; any = true; }
        else { lo = std::min (lo, v); hi = std::max (hi, v); }
    };

    auto scanSamples = [&] (juce::int64 a, juce::int64 b)
    {
        for (int ch = 0; ch < getNumChannels(); ++ch)
        {
            const float* data = samples.getReadPointer (ch);
            for (auto i = a; i < b; ++i)
                addSample (data[i]);
        }
    };

    if (end <= start)
        return { 0.0f, 0.0f };

    const auto firstBlock = (start + kPeakBlock - 1) / kPeakBlock;
    const auto lastBlock = end / kPeakBlock;   // exclusive

    if (lastBlock > firstBlock && ! peaks.empty())
    {
        scanSamples (start, firstBlock * kPeakBlock);

        for (auto b = firstBlock; b < lastBlock && b < (juce::int64) peaks.size(); ++b)
        {
            addSample (peaks[(size_t) b].first);
            addSample (peaks[(size_t) b].second);
        }

        scanSamples (lastBlock * kPeakBlock, end);
    }
    else
    {
        scanSamples (start, end);
    }

    return { lo, hi };
}

void AudioData::buildPeaks()
{
    const auto length = getLength();
    const auto numBlocks = (length + kPeakBlock - 1) / kPeakBlock;
    peaks.assign ((size_t) numBlocks, { 0.0f, 0.0f });

    for (juce::int64 b = 0; b < numBlocks; ++b)
    {
        const auto a = b * kPeakBlock;
        const auto e = std::min (length, a + kPeakBlock);
        float lo = 0.0f, hi = 0.0f;
        bool any = false;

        for (int ch = 0; ch < getNumChannels(); ++ch)
        {
            const float* data = samples.getReadPointer (ch);
            for (auto i = a; i < e; ++i)
            {
                const float v = data[i];
                if (! any) { lo = hi = v; any = true; }
                else { lo = std::min (lo, v); hi = std::max (hi, v); }
            }
        }

        peaks[(size_t) b] = { lo, hi };
    }
}

AudioDataPtr makeAudioData (juce::AudioBuffer<float>&& samples, double sampleRate, const juce::File& file)
{
    auto data = std::make_shared<AudioData>();
    data->file = file;
    data->sampleRate = sampleRate;
    data->samples = std::move (samples);
    data->buildPeaks();
    return data;
}

AudioDataPtr loadAudioFile (juce::AudioFormatManager& formats, const juce::File& file,
                            double targetSampleRate, juce::String& error)
{
    error.clear();

    if (! file.existsAsFile())
    {
        error = utf8 ("파일을 찾을 수 없습니다: ") + file.getFullPathName();
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        error = utf8 ("읽을 수 없는 오디오 파일입니다: ") + file.getFileName();
        return nullptr;
    }

    if (reader->lengthInSamples <= 0 || reader->numChannels <= 0 || reader->sampleRate <= 0.0)
    {
        error = utf8 ("비어 있는 오디오 파일입니다: ") + file.getFileName();
        return nullptr;
    }

    const double ratio = reader->sampleRate / targetSampleRate;   // input samples per output sample
    const auto outLength = (juce::int64) std::ceil ((double) reader->lengthInSamples / ratio);

    if (reader->lengthInSamples > kMaxSamples || outLength > kMaxSamples)
    {
        error = utf8 ("파일이 너무 깁니다: ") + file.getFileName();
        return nullptr;
    }

    const int numChannels = (int) std::min<unsigned int> (2, reader->numChannels);
    const int inLength = (int) reader->lengthInSamples;

    juce::AudioBuffer<float> decoded (numChannels, inLength);
    if (! reader->read (&decoded, 0, inLength, 0, true, numChannels > 1))
    {
        error = utf8 ("오디오 파일을 읽는 중 오류가 났습니다: ") + file.getFileName();
        return nullptr;
    }

    if (std::abs (ratio - 1.0) < 1.0e-9)
        return makeAudioData (std::move (decoded), targetSampleRate, file);

    // Sample-rate conversion. Lagrange interpolation is good enough for a
    // first version; a windowed-sinc converter is a later improvement.
    juce::AudioBuffer<float> converted (numChannels, (int) outLength);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, decoded.getReadPointer (ch), converted.getWritePointer (ch),
                              (int) outLength, inLength, 0);
    }

    return makeAudioData (std::move (converted), targetSampleRate, file);
}
