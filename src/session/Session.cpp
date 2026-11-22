#include "Session.h"

#include <algorithm>

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

constexpr int kRecordFifoSamples = 1 << 18;   // about 5 s at 48 kHz per take

const juce::uint32 kTrackColours[] = { 0xff4f8fd6, 0xff58b368, 0xffd6a24f, 0xffc1584f, 0xff9a6bd1,
                                       0xff4fbfc1, 0xffd16ba7, 0xff8fa84a };

struct ClipEditAction : public juce::UndoableAction
{
    ClipEditAction (Session& s, Session::ClipMap b, Session::ClipMap a)
        : session (s), before (std::move (b)), after (std::move (a)) {}

    bool perform() override { session.applyClips (after); return true; }
    bool undo() override    { session.applyClips (before); return true; }
    int getSizeInUnits() override
    {
        size_t n = 0;
        for (const auto& [id, clips] : before) n += clips.size();
        for (const auto& [id, clips] : after) n += clips.size();
        return (int) n + 1;
    }

    Session& session;
    Session::ClipMap before, after;
};

juce::String safeFileName (const juce::String& name)
{
    auto s = juce::File::createLegalFileName (name).trim();
    return s.isEmpty() ? juce::String ("Audio") : s;
}
} // namespace

//==============================================================================
Track::Track (int trackId, MixEngine& engine, juce::AudioPluginFormatManager& formats,
              PluginChain::ProcessorFactory factory)
    : id (trackId), audio (engine.createTrackAudio())
{
    inserts = std::make_unique<PluginChain> (audio->getChain(), formats, std::move (factory));
    pushParameters();
}

void Track::pushParameters()
{
    audio->setVolumeDb (volumeDb);
    audio->setPan (pan);
    audio->setMute (mute);
    audio->setSolo (solo);
    audio->setArmed (armed);
    audio->setInput (inputChannel, stereoInput);
}

//==============================================================================
Session::Session (MixEngine& e, juce::AudioPluginFormatManager& pf, juce::AudioFormatManager& af,
                  PluginChain::ProcessorFactory factory)
    : engine (e), pluginFormats (pf), audioFormats (af), processorFactory (std::move (factory))
{
    masterInserts = std::make_unique<PluginChain> (engine.getMaster().getChain(), pluginFormats, processorFactory);
    attachChain (*masterInserts);
    engine.getMaster().setVolumeDb (masterVolumeDb);
    startTimerHz (30);
    publish();
}

Session::~Session()
{
    stopTimer();
    undoManager.clearUndoHistory();

    // Tracks go first (their PluginChains, then their TrackAudio references),
    // then an empty snapshot drops the engine's references too.
    tracks.clear();
    masterInserts.reset();
    engine.publish (std::make_unique<PlaybackSnapshot>());
}

void Session::attachChain (PluginChain& chain)
{
    chain.onChanged = [this]
    {
        if (onInsertsChanged)
            onInsertsChanged();
        setDirty (true);
        sendChangeMessage();
    };
    chain.onError = [this] (const juce::String& message) { if (onError) onError (message); };
}

void Session::reset (double newSampleRate)
{
    engine.stop();
    undoManager.clearUndoHistory();

    while (! tracks.empty())
        removeTrack (tracks.back()->id);

    for (const auto& entry : std::vector<ChainEntry> (masterInserts->getEntries()))
        masterInserts->remove (entry.id);

    setMasterVolume (0.0f);
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    tempo = 120.0;
    engine.setTempo (tempo);
    file = juce::File();
    // Track and clip ids keep counting: an id never names two different
    // tracks, so late results (a take of the old session's recording) or
    // UI rows of the old session can never attach to the new one.
    colourIndex = 0;
    engine.locate (0);
    publish();
    dirty = false;
    sendChangeMessage();
}

bool Session::adoptSampleRate (double rate)
{
    if (rate <= 0.0)
        return false;

    for (const auto& track : tracks)
        if (! track->clips.empty())
            return juce::exactlyEqual (rate, sampleRate);

    if (! juce::exactlyEqual (rate, sampleRate))
        undoManager.clearUndoHistory();   // undo would bring back audio at the old rate

    sampleRate = rate;
    return true;
}

void Session::setTempo (double bpm)
{
    tempo = juce::jlimit (20.0, 400.0, bpm);
    engine.setTempo (tempo);
    changed();
}

void Session::setFile (const juce::File& f)
{
    file = f;
    sendChangeMessage();
}

juce::String Session::getName() const
{
    return file == juce::File() ? utf8 ("제목 없음") : file.getFileNameWithoutExtension();
}

void Session::setDirty (bool d)
{
    if (dirty != d)
    {
        dirty = d;
        sendChangeMessage();
    }
}

juce::File Session::getAudioFolder() const
{
    if (file != juce::File())
        return file.getParentDirectory().getChildFile ("Audio Files");

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("DAW").getChildFile ("Unsaved Recordings");
}

void Session::changed()
{
    dirty = true;
    publish();
    sendChangeMessage();
}

void Session::publish()
{
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    for (const auto& track : tracks)
        snapshot->addTrack (track->getAudioPtr(), track->clips);

    engine.publish (std::move (snapshot));
}

juce::Colour Session::nextColour()
{
    const auto n = (int) (sizeof (kTrackColours) / sizeof (kTrackColours[0]));
    return juce::Colour (kTrackColours[colourIndex++ % n]);
}

//==============================================================================
Track* Session::findTrack (int id) const
{
    for (const auto& track : tracks)
        if (track->id == id)
            return track.get();

    return nullptr;
}

int Session::indexOfTrack (int id) const
{
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->id == id)
            return (int) i;

    return -1;
}

Track* Session::addTrack (const juce::String& name)
{
    if ((int) tracks.size() >= kMaxTracks)
    {
        if (onError)
            onError (utf8 ("트랙은 최대 64개까지 만들 수 있습니다."));
        return nullptr;
    }

    const int id = nextTrackId++;
    auto track = std::make_unique<Track> (id, engine, pluginFormats, processorFactory);
    track->name = name.isNotEmpty() ? name : utf8 ("오디오 ") + juce::String (id);
    track->colour = nextColour();
    track->inputChannel = 0;
    track->pushParameters();
    attachChain (track->getInserts());

    auto* raw = track.get();
    tracks.push_back (std::move (track));
    changed();
    return raw;
}

void Session::removeTrack (int id)
{
    const int index = indexOfTrack (id);
    if (index < 0)
        return;

    if (onTrackRemoving)
        onTrackRemoving (id);

    tracks.erase (tracks.begin() + index);
    changed();   // the new snapshot drops the engine's reference
}

void Session::moveTrack (int id, int newIndex)
{
    const int index = indexOfTrack (id);
    if (index < 0)
        return;

    newIndex = juce::jlimit (0, (int) tracks.size() - 1, newIndex);
    if (newIndex == index)
        return;

    auto track = std::move (tracks[(size_t) index]);
    tracks.erase (tracks.begin() + index);
    tracks.insert (tracks.begin() + newIndex, std::move (track));
    changed();
}

void Session::renameTrack (int id, const juce::String& name)
{
    if (auto* track = findTrack (id); track != nullptr && name.trim().isNotEmpty())
    {
        track->name = name.trim();
        setDirty (true);
        sendChangeMessage();
    }
}

#define DAW_SET_TRACK_PARAM(field, value)                \
    if (auto* track = findTrack (id))                     \
    {                                                     \
        track->field = value;                             \
        track->pushParameters();                          \
        setDirty (true);                                  \
        sendChangeMessage();                              \
    }

void Session::setTrackVolume (int id, float db)     { DAW_SET_TRACK_PARAM (volumeDb, juce::jlimit (TrackAudio::kMinDb, 12.0f, db)) }
void Session::setTrackPan (int id, float p)         { DAW_SET_TRACK_PARAM (pan, juce::jlimit (-1.0f, 1.0f, p)) }
void Session::setTrackMute (int id, bool m)         { DAW_SET_TRACK_PARAM (mute, m) }
void Session::setTrackSolo (int id, bool s)         { DAW_SET_TRACK_PARAM (solo, s) }
void Session::setTrackArmed (int id, bool a)
{
    if (engine.isRecording())
        return;   // the takes of the pass in progress are fixed
    DAW_SET_TRACK_PARAM (armed, a)
}

void Session::setTrackInput (int id, int firstChannel, bool stereo)
{
    if (auto* track = findTrack (id))
    {
        track->inputChannel = std::max (0, firstChannel);
        track->stereoInput = stereo;
        track->pushParameters();
        setDirty (true);
        sendChangeMessage();
    }
}

#undef DAW_SET_TRACK_PARAM

void Session::setMasterVolume (float db)
{
    masterVolumeDb = juce::jlimit (TrackAudio::kMinDb, 12.0f, db);
    engine.getMaster().setVolumeDb (masterVolumeDb);
    setDirty (true);
    sendChangeMessage();
}

//==============================================================================
const Clip* Session::findClip (int clipId, int* trackId) const
{
    for (const auto& track : tracks)
    {
        const int index = ClipEdit::indexOf (track->clips, clipId);
        if (index >= 0)
        {
            if (trackId != nullptr)
                *trackId = track->id;
            return &track->clips[(size_t) index];
        }
    }

    return nullptr;
}

Session::ClipMap Session::captureClips() const
{
    ClipMap map;
    for (const auto& track : tracks)
        map[track->id] = track->clips;
    return map;
}

void Session::applyClips (const ClipMap& map)
{
    for (const auto& [id, clips] : map)
    {
        if (auto* track = findTrack (id))
        {
            track->clips = clips;
            ClipEdit::sort (track->clips);
        }
    }

    changed();
}

void Session::commitClipEdit (const juce::String& actionName, const ClipMap& before)
{
    auto after = captureClips();

    // Only tracks that changed (and still exist) go into the step.
    ClipMap b, a;
    for (const auto& [id, clips] : after)
    {
        const auto it = before.find (id);
        if (it == before.end() || it->second != clips)
        {
            a[id] = clips;
            b[id] = it != before.end() ? it->second : std::vector<Clip>();
        }
    }

    if (a.empty())
        return;

    undoManager.beginNewTransaction (actionName);
    undoManager.perform (new ClipEditAction (*this, std::move (b), std::move (a)), actionName);
}

void Session::editClips (const juce::String& actionName, const std::function<void (ClipMap&)>& edit)
{
    const auto before = captureClips();
    auto working = before;
    edit (working);
    applyClips (working);
    commitClipEdit (actionName, before);
}

int Session::importAudioFile (const juce::File& audioFile, int trackId, juce::int64 position, juce::String& error)
{
    auto* track = findTrack (trackId);
    if (track == nullptr)
    {
        error = utf8 ("가져올 트랙이 없습니다.");
        return 0;
    }

    // A file already in the session is shared, not loaded again.
    AudioDataPtr data;
    for (const auto& t : tracks)
        for (const auto& c : t->clips)
            if (data == nullptr && c.data != nullptr && c.data->file == audioFile)
                data = c.data;

    if (data == nullptr)
        data = loadAudioFile (audioFormats, audioFile, sampleRate, error);
    if (data == nullptr)
        return 0;

    Clip clip;
    clip.id = newClipId();
    clip.name = audioFile.getFileNameWithoutExtension();
    clip.data = data;
    clip.start = std::max<juce::int64> (0, position);
    clip.length = data->getLength();

    editClips (utf8 ("오디오 가져오기"), [&] (ClipMap& map)
    {
        ClipEdit::place (map[trackId], clip, idSource());
    });

    return clip.id;
}

void Session::splitClips (const std::vector<int>& trackIds, juce::int64 position)
{
    editClips (utf8 ("분할"), [&] (ClipMap& map)
    {
        for (const int id : trackIds)
            if (map.count (id) != 0)
                ClipEdit::splitAt (map[id], position, idSource());
    });
}

void Session::deleteClips (const std::set<int>& clipIds)
{
    editClips (utf8 ("클립 삭제"), [&] (ClipMap& map)
    {
        for (auto& [id, clips] : map)
            clips.erase (std::remove_if (clips.begin(), clips.end(),
                                         [&] (const Clip& c) { return clipIds.count (c.id) != 0; }),
                         clips.end());
    });
}

void Session::deleteRange (const std::vector<int>& trackIds, juce::int64 from, juce::int64 to)
{
    editClips (utf8 ("구간 삭제"), [&] (ClipMap& map)
    {
        for (const int id : trackIds)
            if (map.count (id) != 0)
                ClipEdit::clearRange (map[id], from, to, idSource());
    });
}

std::set<int> Session::duplicateClips (const std::set<int>& clipIds)
{
    std::set<int> created;

    editClips (utf8 ("복제"), [&] (ClipMap& map)
    {
        // The selection moves as one block, across all its tracks: every copy
        // keeps its distance from the earliest clip and starts after the end
        // of the latest one.
        bool any = false;
        juce::int64 first = 0, last = 0;
        for (const auto& [id, clips] : map)
            for (const auto& c : clips)
                if (clipIds.count (c.id) != 0)
                {
                    first = any ? std::min (first, c.start) : c.start;
                    last = any ? std::max (last, c.end()) : c.end();
                    any = true;
                }

        if (! any)
            return;

        for (auto& [id, clips] : map)
        {
            std::vector<Clip> sources;   // copied first: placing changes the list
            for (const auto& c : clips)
                if (clipIds.count (c.id) != 0)
                    sources.push_back (c);

            for (auto copy : sources)
            {
                copy.id = newClipId();
                copy.start += last - first;
                created.insert (copy.id);
                ClipEdit::place (clips, copy, idSource());
            }
        }
    });

    return created;
}

juce::int64 Session::getLength() const
{
    juce::int64 end = 0;
    for (const auto& track : tracks)
        for (const auto& clip : track->clips)
            end = std::max (end, clip.end());
    return end;
}

//==============================================================================
juce::String Session::startRecording()
{
    if (! engine.isDeviceRunning())
        return utf8 ("오디오 장치가 열려 있지 않아 녹음할 수 없습니다. [설정] → [오디오 설정]에서 장치를 고르세요.");

    if (engine.isRecording())
        return {};

    auto job = std::make_unique<RecordJob>();
    const auto folder = getAudioFolder();

    if (! folder.createDirectory())
        return utf8 ("녹음 폴더를 만들 수 없습니다: ") + folder.getFullPathName();

    juce::WavAudioFormat wav;

    // On failure the files made so far are closed and deleted.
    auto fail = [&job] (const juce::String& message)
    {
        for (auto& t : job->takes)
        {
            t.writer.reset();
            t.file.deleteFile();
        }
        return message;
    };

    for (const auto& track : tracks)
    {
        if (! track->armed)
            continue;

        RecordTake take;
        take.trackId = track->id;
        take.firstChannel = track->inputChannel;
        take.numChannels = track->stereoInput ? 2 : 1;
        take.file = folder.getNonexistentChildFile (safeFileName (track->name) + "_01", ".wav", false);

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take.file);
        if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
            return fail (utf8 ("녹음 파일을 만들 수 없습니다: ") + take.file.getFullPathName());

        const auto options = juce::AudioFormatWriterOptions()
                                 .withSampleRate (sampleRate)
                                 .withNumChannels (take.numChannels)
                                 .withBitsPerSample (24);
        auto writer = wav.createWriterFor (stream, options);
        if (writer == nullptr)
        {
            stream.reset();
            take.file.deleteFile();
            return fail (utf8 ("녹음 파일을 만들 수 없습니다: ") + take.file.getFullPathName());
        }

        take.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), engine.getWriterThread(),
                                                                                kRecordFifoSamples);
        job->takes.push_back (std::move (take));
    }

    if (job->takes.empty())
        return utf8 ("녹음할 트랙이 없습니다. 트랙의 [R] 버튼으로 녹음 준비를 먼저 하세요.");

    std::vector<juce::File> files;
    for (const auto& t : job->takes)
        files.push_back (t.file);

    if (! engine.startRecording (std::move (job)))   // the job (and its writers) is gone now
    {
        for (const auto& f : files)
            f.deleteFile();
        return utf8 ("녹음을 시작할 수 없습니다.");
    }

    return {};
}

void Session::timerCallback()
{
    engine.collectGarbage();

    // A clip drag rebuilds the clips from its start state on every move:
    // takes wait until the mouse is released.
    if (gestureActive)
        return;

    while (auto job = engine.popFinishedRecording())
        collectRecording (*job);
}

void Session::collectRecording (RecordJob& job)
{
    bool overflow = false;
    std::vector<std::pair<int, Clip>> newClips;

    for (auto& take : job.takes)
    {
        take.writer.reset();   // flushes the FIFO and closes the file
        overflow = overflow || take.overflowed;

        const auto usable = take.samplesWritten - job.latencySamples;
        if (usable <= 0 || findTrack (take.trackId) == nullptr)
        {
            take.file.deleteFile();
            continue;
        }

        juce::String error;
        auto data = loadAudioFile (audioFormats, take.file, sampleRate, error);
        if (data == nullptr)
        {
            if (onError)
                onError (error);
            continue;
        }

        Clip clip;
        clip.id = newClipId();
        clip.name = take.file.getFileNameWithoutExtension();
        clip.data = data;
        clip.start = job.startPosition;
        clip.offset = std::min<juce::int64> (job.latencySamples, data->getLength() - 1);
        clip.length = std::max<juce::int64> (1, data->getLength() - clip.offset);
        newClips.emplace_back (take.trackId, clip);
    }

    if (! newClips.empty())
    {
        editClips (utf8 ("녹음"), [&] (ClipMap& map)
        {
            for (auto& [trackId, clip] : newClips)
                ClipEdit::place (map[trackId], clip, idSource());
        });
    }

    if (overflow && onError)
        onError (utf8 ("디스크 쓰기가 늦어 녹음 일부가 빠졌을 수 있습니다."));
}
