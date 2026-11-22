#include "MixEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

constexpr double kClickSeconds = 0.030;
constexpr float kClickLevel = 0.35f;
} // namespace

//==============================================================================
void PlaybackSnapshot::addTrack (std::shared_ptr<TrackAudio> audio, const std::vector<Clip>& clips)
{
    TrackPlayback track;
    track.audio = audio.get();
    track.clips.reserve (clips.size());

    for (const auto& clip : clips)
    {
        if (clip.data == nullptr || clip.length <= 0 || clip.offset < 0)
            continue;

        ClipPlayback c;
        c.numChannels = std::min (2, clip.data->getNumChannels());
        if (c.numChannels <= 0)
            continue;

        for (int ch = 0; ch < c.numChannels; ++ch)
            c.channels[ch] = clip.data->samples.getReadPointer (ch);

        c.start = clip.start;
        c.offset = clip.offset;
        c.length = std::min (clip.length, clip.data->getLength() - clip.offset);   // never read past the data
        if (c.length <= 0)
            continue;

        c.fadeIn = std::min (clip.fadeIn, c.length);
        c.fadeOut = std::min (clip.fadeOut, c.length);
        c.gain = juce::Decibels::decibelsToGain (clip.gainDb, -100.0f);
        track.clips.push_back (c);
        keepData.push_back (clip.data);
    }

    std::sort (track.clips.begin(), track.clips.end(),
               [] (const ClipPlayback& a, const ClipPlayback& b) { return a.start < b.start; });

    tracks.push_back (std::move (track));
    keepTracks.push_back (std::move (audio));
}

//==============================================================================
struct MixEngine::Registry
{
    std::mutex mutex;
    std::vector<TrackAudio*> tracks;
    double rate = 0.0;
    int block = 0;
    bool active = false;
};

MixEngine::MixEngine() : registry (std::make_shared<Registry>())
{
    master = createTrackAudio();
    writerThread.startThread();
}

MixEngine::~MixEngine()
{
    shutdown();
    writerThread.stopThread (2000);

    delete pending.exchange (nullptr);
    delete current;
    current = nullptr;
    collectGarbage();

    delete pendingRecord.exchange (nullptr);
    delete activeRecord;
    RecordJob* job = nullptr;
    while (finishedRecords.pop (job))
        delete job;
}

juce::String MixEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    auto error = deviceManager.initialise (kMaxInputChannels, 2, savedDeviceState, true);

    // As in the broadcast app before: when the saved device (for example an
    // unplugged ASIO interface) cannot open, open the default device of the
    // first other device type that has devices, without forgetting the choice.
    if (savedDeviceState != nullptr)
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        const auto savedOutput = savedDeviceState->getStringAttribute ("audioOutputDeviceName");
        const bool wrongAsioDriver = device != nullptr
                                     && deviceManager.getCurrentAudioDeviceType() == "ASIO"
                                     && savedOutput.isNotEmpty() && device->getName() != savedOutput;

        if (device == nullptr || wrongAsioDriver)
        {
            const auto currentType = deviceManager.getCurrentAudioDeviceType();

            for (auto* type : deviceManager.getAvailableDeviceTypes())
            {
                if (type->getTypeName() == currentType || type->getTypeName() == "ASIO")
                    continue;

                type->scanForDevices();
                if (type->getDeviceNames (false).isEmpty())
                    continue;

                deviceManager.setCurrentAudioDeviceType (type->getTypeName(), false);
                if (deviceManager.getCurrentAudioDevice() != nullptr)
                {
                    error.clear();
                    break;
                }
            }
        }
    }

    deviceManager.addAudioCallback (this);
    callbackAttached = true;

    if (error.isNotEmpty())
        return utf8 ("오디오 장치를 열 수 없습니다: ") + error;

    return {};
}

void MixEngine::shutdown()
{
    if (callbackAttached)
    {
        deviceManager.removeAudioCallback (this);
        callbackAttached = false;
    }

    deviceManager.closeAudioDevice();
    audioDeviceStopped();
}

//==============================================================================
std::shared_ptr<TrackAudio> MixEngine::createTrackAudio()
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto reg = registry;
    auto* track = new TrackAudio();

    {
        const std::lock_guard<std::mutex> lock (reg->mutex);
        if (reg->rate > 0.0)
            track->prepare (reg->rate, reg->block);

        track->getChain().setProcessingActive (reg->active);

        // Start at the delay the other tracks are compensated to, so a new
        // track joins without a compensation change.
        if (master != nullptr)
            track->setCompensation (std::max (0, mixLatency.load() - master->getChain().getTotalLatencySamples()));

        reg->tracks.push_back (track);
    }

    return std::shared_ptr<TrackAudio> (track, [reg] (TrackAudio* t)
    {
        {
            const std::lock_guard<std::mutex> lock (reg->mutex);
            reg->tracks.erase (std::remove (reg->tracks.begin(), reg->tracks.end(), t), reg->tracks.end());
        }
        delete t;
    });
}

void MixEngine::prepareAll (double sampleRate, int blockSize)
{
    blockSize = std::max (1, blockSize);
    {
        const std::lock_guard<std::mutex> lock (registry->mutex);
        registry->rate = sampleRate;
        registry->block = blockSize;
        registry->active = true;

        for (auto* track : registry->tracks)
        {
            track->prepare (sampleRate, blockSize);
            track->getChain().setProcessingActive (true);
        }
    }

    silence.setSize (1, blockSize);
    silence.clear();
    pdcGain.reset (sampleRate, kPdcDipSeconds);
    pdcGain.setCurrentAndTargetValue (1.0f);
    pdcHoldSamples = 0;
    preparedRate.store (sampleRate);
    preparedBlock.store (blockSize);
}

void MixEngine::stopAll()
{
    preparedBlock.store (0);
    {
        const std::lock_guard<std::mutex> lock (registry->mutex);
        registry->active = false;

        for (auto* track : registry->tracks)
            track->getChain().setProcessingActive (false);
    }

    // IO is stopped: hand back a recording in progress and stop the transport.
    finishRecording();
    playRequested.store (false);
    playing.store (false);
}

void MixEngine::prepareForTest (double sampleRate, int blockSize)
{
    JUCE_ASSERT_MESSAGE_THREAD
    prepareAll (sampleRate, blockSize);
    deviceRunning.store (true);
}

void MixEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr || offline.load())
        return;

    prepareAll (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    deviceRunning.store (true);
}

void MixEngine::audioDeviceStopped()
{
    deviceRunning.store (false);
    stopAll();
    master->meterL.readAndReset();
    master->meterR.readAndReset();
}

void MixEngine::audioDeviceError (const juce::String&)
{
    // The device may keep calling back after an error, so nothing is torn
    // down here: if it really stops, JUCE calls audioDeviceStopped().
}

//==============================================================================
void MixEngine::publish (std::unique_ptr<PlaybackSnapshot> snapshot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    // A snapshot still pending was never seen by the audio thread.
    delete pending.exchange (snapshot.release());
}

void MixEngine::collectGarbage()
{
    PlaybackSnapshot* old = nullptr;
    while (retired.pop (old))
        delete old;
}

void MixEngine::takeSnapshot() noexcept
{
    // Only when the old one can be handed back: nothing is ever freed here.
    if (retired.getNumReady() >= retired.capacity())
        return;

    if (auto* next = pending.exchange (nullptr))
    {
        if (current != nullptr)
            retired.push (current);

        current = next;
    }
}

//==============================================================================
bool MixEngine::play()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! deviceRunning.load())
        return false;

    playRequested.store (true);
    return true;
}

void MixEngine::stop()
{
    JUCE_ASSERT_MESSAGE_THREAD
    playRequested.store (false);

    // A recording the audio thread never picked up comes back unused.
    if (auto* job = pendingRecord.exchange (nullptr))
        reclaimedRecords.emplace_back (job);
}

void MixEngine::locate (juce::int64 newPosition)
{
    JUCE_ASSERT_MESSAGE_THREAD
    // Not while a take is being written. Once stop() was called the callback
    // applies the locate in the same block that finishes the take.
    if (isRecording() && playRequested.load())
        return;

    newPosition = std::max<juce::int64> (0, newPosition);

    if (deviceRunning.load())
        locateTarget.store (newPosition);
    else
        position.store (newPosition);
}

void MixEngine::setLoop (bool enabled, juce::int64 start, juce::int64 end)
{
    loopStart.store (std::max<juce::int64> (0, start));
    loopEnd.store (std::max<juce::int64> (0, end));
    loopEnabled.store (enabled && end > start);
}

bool MixEngine::startRecording (std::unique_ptr<RecordJob> job)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (job == nullptr || ! deviceRunning.load() || isRecording())
        return false;

    job->latencySamples = getRecordingLatency();
    RecordJob* expected = nullptr;
    if (! pendingRecord.compare_exchange_strong (expected, job.get()))
        return false;

    job.release();
    playRequested.store (true);
    return true;
}

std::unique_ptr<RecordJob> MixEngine::popFinishedRecording()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! reclaimedRecords.empty())
    {
        auto job = std::move (reclaimedRecords.back());
        reclaimedRecords.pop_back();
        return job;
    }

    RecordJob* job = nullptr;
    return finishedRecords.pop (job) ? std::unique_ptr<RecordJob> (job) : nullptr;
}

int MixEngine::getRecordingLatency()
{
    int latency = getMixLatency();

    if (auto* device = deviceManager.getCurrentAudioDevice())
        latency += device->getInputLatencyInSamples() + device->getOutputLatencyInSamples();

    return latency;
}

void MixEngine::finishRecording() noexcept
{
    if (activeRecord != nullptr && finishedRecords.push (activeRecord))
        activeRecord = nullptr;

    recording.store (activeRecord != nullptr);
}

//==============================================================================
void MixEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                  float* const* outputs, int numOutputs,
                                                  int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    const int block = preparedBlock.load();
    if (block <= 0 || offline.load())
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);
        return;
    }

    takeSnapshot();

    if (const auto target = locateTarget.exchange (-1); target >= 0)
        position.store (target);

    const bool wantPlay = playRequested.load();
    juce::int64 pos = position.load();

    if (activeRecord == nullptr && wantPlay)
    {
        if (auto* job = pendingRecord.exchange (nullptr))
        {
            job->startPosition = pos;
            activeRecord = job;
            recording.store (true);
        }
    }

    if (! wantPlay && activeRecord != nullptr)
        finishRecording();

    playing.store (wantPlay);

    const bool isRec = activeRecord != nullptr && wantPlay;
    const bool loop = wantPlay && ! isRec && loopEnabled.load();
    const auto ls = loopStart.load(), le = loopEnd.load();

    for (int start = 0; start < numSamples;)
    {
        int n = std::min (block, numSamples - start);

        if (loop && le > ls)
        {
            if (pos >= le)
                pos = ls;
            n = (int) std::min<juce::int64> (n, le - pos);
        }

        renderChunk (pos, n, wantPlay, isRec, inputs, numInputs, start, true);

        auto* const* mix = master->getBuffer();
        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr)
                continue;

            if (ch < 2)
                juce::FloatVectorOperations::copy (outputs[ch] + start, mix[ch], n);
            else
                juce::FloatVectorOperations::clear (outputs[ch] + start, n);
        }

        if (wantPlay)
            pos += n;

        start += n;
    }

    if (wantPlay)
        position.store (pos);
}

void MixEngine::updateCompensation() noexcept
{
    int maxLatency = 0;
    for (const auto& track : current->tracks)
        maxLatency = std::max (maxLatency, track.audio->getChain().getTotalLatencySamples());
    maxLatency = std::min (maxLatency, TrackAudio::kMaxCompensation);

    mixLatency.store (maxLatency + master->getChain().getTotalLatencySamples());

    bool changed = false;
    for (const auto& track : current->tracks)
    {
        const int wanted = std::max (0, maxLatency - track.audio->getChain().getTotalLatencySamples());
        changed = changed || track.audio->getCompensation() != wanted;
    }

    if (! changed)
    {
        if (pdcHoldSamples == 0)
            pdcGain.setTargetValue (1.0f);
        return;
    }

    // Changing a delay line's length jumps the signal: dip the whole mix to 0
    // first, switch every track, keep the output silent until the longer
    // delays are filled with fresh audio, then fade back in.
    if (pdcGain.getCurrentValue() > 0.0f || pdcGain.isSmoothing())
    {
        pdcGain.setTargetValue (0.0f);
        return;
    }

    int hold = 0;
    for (const auto& track : current->tracks)
    {
        const int wanted = std::max (0, maxLatency - track.audio->getChain().getTotalLatencySamples());
        hold = std::max (hold, wanted - track.audio->getCompensation());
        track.audio->setCompensation (wanted);
    }
    pdcHoldSamples = hold;
}

void MixEngine::renderChunk (juce::int64 pos, int n, bool isPlayingNow, bool isRecordingNow,
                             const float* const* inputs, int numInputs, int inputOffset, bool withClick) noexcept
{
    auto* const* mix = master->getBuffer();
    juce::FloatVectorOperations::clear (mix[0], n);
    juce::FloatVectorOperations::clear (mix[1], n);

    if (current != nullptr)
    {
        updateCompensation();

        bool anySolo = false;
        for (const auto& track : current->tracks)
            anySolo = anySolo || track.audio->isSoloed();

        for (const auto& track : current->tracks)
        {
            auto& audio = *track.audio;
            auto* const* buf = audio.getBuffer();
            juce::FloatVectorOperations::clear (buf[0], n);
            juce::FloatVectorOperations::clear (buf[1], n);

            // Auto input monitoring: an armed track plays its input while
            // stopped or recording, and its clips during normal playback.
            // Input monitoring ("I") plays the input all the time.
            const bool monitor = inputs != nullptr
                                 && (audio.isInputMonitor() || (audio.isArmed() && (! isPlayingNow || isRecordingNow)));

            if (monitor)
            {
                const int first = audio.getInputChannel();
                const int second = audio.isInputStereo() ? first + 1 : first;

                if (first >= 0 && first < numInputs && inputs[first] != nullptr)
                    juce::FloatVectorOperations::copy (buf[0], inputs[first] + inputOffset, n);
                if (second >= 0 && second < numInputs && inputs[second] != nullptr)
                    juce::FloatVectorOperations::copy (buf[1], inputs[second] + inputOffset, n);
            }
            else if (isPlayingNow)
            {
                const auto chunkEnd = pos + n;

                for (const auto& clip : track.clips)
                {
                    if (clip.start >= chunkEnd)
                        break;   // sorted: nothing further overlaps

                    const auto clipEnd = clip.start + clip.length;
                    if (clipEnd <= pos)
                        continue;

                    const auto from = std::max (pos, clip.start);
                    const auto to = std::min (chunkEnd, clipEnd);
                    const float* left = clip.channels[0];
                    const float* right = clip.numChannels > 1 ? clip.channels[1] : clip.channels[0];

                    for (auto t = from; t < to; ++t)
                    {
                        const auto k = t - clip.start;
                        float g = clip.gain;
                        if (k < clip.fadeIn)
                            g *= (float) k / (float) clip.fadeIn;
                        if (k >= clip.length - clip.fadeOut)
                            g *= (float) (clip.length - k) / (float) clip.fadeOut;

                        const auto src = clip.offset + k;
                        const int i = (int) (t - pos);
                        buf[0][i] += left[src] * g;
                        buf[1][i] += right[src] * g;
                    }
                }
            }

            audio.processInserts (n);
            audio.mixInto (mix, n, anySolo && ! audio.isSoloed());
        }
    }

    if (isRecordingNow)
        writeTakes (inputs, numInputs, inputOffset, n);

    master->processInserts (n);
    master->applyFaderInPlace (n);

    for (int i = 0; i < n; ++i)
    {
        float g;
        if (pdcHoldSamples > 0)
        {
            --pdcHoldSamples;
            g = 0.0f;
        }
        else
        {
            g = pdcGain.getNextValue();
        }

        if (g < 1.0f)
        {
            mix[0][i] *= g;
            mix[1][i] *= g;
        }
    }

    if (withClick && isPlayingNow && clickEnabled.load())
        addClick (pos, n);
}

void MixEngine::writeTakes (const float* const* inputs, int numInputs, int inputOffset, int n) noexcept
{
    if (activeRecord == nullptr)
        return;

    for (auto& take : activeRecord->takes)
    {
        const float* channels[2] = { silence.getReadPointer (0), silence.getReadPointer (0) };

        for (int c = 0; c < std::min (2, take.numChannels); ++c)
        {
            const int ch = take.firstChannel + c;
            if (inputs != nullptr && ch >= 0 && ch < numInputs && inputs[ch] != nullptr)
                channels[c] = inputs[ch] + inputOffset;
        }

        if (take.writer != nullptr && take.writer->write (channels, n))
            take.samplesWritten += n;
        else
            take.overflowed = true;
    }
}

void MixEngine::addClick (juce::int64 pos, int n) noexcept
{
    const double rate = preparedRate.load();
    if (rate <= 0.0)
        return;

    const double beat = rate * 60.0 / tempo.load();
    const double clickLength = rate * kClickSeconds;
    auto* const* mix = master->getBuffer();

    for (int i = 0; i < n; ++i)
    {
        const double s = (double) (pos + i);
        const double beatIndex = std::floor (s / beat);
        const double phase = s - beatIndex * beat;

        if (phase >= clickLength)
            continue;

        const double frequency = ((juce::int64) beatIndex % 4 == 0) ? 1500.0 : 1000.0;
        const float envelope = 1.0f - (float) (phase / clickLength);
        const float value = kClickLevel * envelope * envelope
                            * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * phase / rate);
        mix[0][i] += value;
        mix[1][i] += value;
    }
}

//==============================================================================
void MixEngine::beginOffline (double sampleRate, int blockSize)
{
    JUCE_ASSERT_MESSAGE_THREAD
    stop();

    if (callbackAttached)
    {
        deviceManager.removeAudioCallback (this);   // stops: audioDeviceStopped()
        callbackAttached = false;
    }

    audioDeviceStopped();
    offline.store (true);
    prepareAll (sampleRate, blockSize);
}

void MixEngine::renderOffline (juce::int64 pos, juce::AudioBuffer<float>& out)
{
    const int n = std::min (out.getNumSamples(), preparedBlock.load());
    takeSnapshot();
    renderChunk (pos, n, true, false, nullptr, 0, 0, false);

    auto* const* mix = master->getBuffer();
    for (int ch = 0; ch < std::min (2, out.getNumChannels()); ++ch)
        out.copyFrom (ch, 0, mix[ch], n);
}

void MixEngine::endOffline()
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopAll();
    preparedRate.store (0.0);
    offline.store (false);

    deviceManager.addAudioCallback (this);   // restarts: audioDeviceAboutToStart()
    callbackAttached = true;
}
