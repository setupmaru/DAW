#include "SessionFile.h"

#include <map>
#include <set>

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

std::unique_ptr<juce::XmlElement> parse (const juce::File& file, juce::String& error)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("DAWSession"))
    {
        error = utf8 ("세션 파일을 읽을 수 없습니다: ") + file.getFileName();
        return nullptr;
    }

    if (xml->getIntAttribute ("version") != 1)
    {
        error = utf8 ("이 버전에서 열 수 없는 세션 파일입니다: ") + file.getFileName();
        return nullptr;
    }

    return xml;
}

// Brings one audio file into the session's Audio Files folder. Returns the
// file to use, or an empty File on failure.
juce::File bringIntoFolder (const juce::File& source, const juce::File& audioFolder, const juce::File& unsavedFolder)
{
    if (source.isAChildOf (audioFolder))
        return source;

    auto target = audioFolder.getChildFile (source.getFileName());
    if (target.exists())
        target = audioFolder.getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), false);

    // Recordings of a never-saved session are moved, everything else copied.
    const bool ok = source.isAChildOf (unsavedFolder) ? source.moveFileTo (target) : source.copyFileTo (target);
    return ok ? target : juce::File();
}

// Writes audio that is only in memory (its file is gone or unreadable) as a
// 32-bit float WAV, so nothing is lost. Empty File on failure.
juce::File writeFromMemory (const AudioData& data, const juce::File& audioFolder, const juce::String& name)
{
    const auto target = audioFolder.getNonexistentChildFile (juce::File::createLegalFileName (name.isNotEmpty() ? name : "Audio"),
                                                             ".wav", false);
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
        return {};

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                   .withSampleRate (data.sampleRate)
                                                   .withNumChannels (data.getNumChannels())
                                                   .withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (data.samples, 0, data.samples.getNumSamples()))
    {
        writer.reset();
        stream.reset();
        target.deleteFile();
        return {};
    }

    return target;
}
} // namespace

namespace SessionFile
{
double readSampleRate (const juce::File& file)
{
    juce::String error;
    const auto xml = parse (file, error);
    return xml != nullptr ? xml->getDoubleAttribute ("sampleRate") : 0.0;
}

juce::String save (Session& session, const juce::File& target)
{
    const auto folder = target.getParentDirectory();
    const auto audioFolder = folder.getChildFile ("Audio Files");
    const auto unsavedFolder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                   .getChildFile ("DAW").getChildFile ("Unsaved Recordings");

    if (! audioFolder.createDirectory())
        return utf8 ("오디오 폴더를 만들 수 없습니다: ") + audioFolder.getFullPathName();

    // 1. Every clip's audio into the session folder (each data once).
    // Several clips (and several AudioData objects) may use the same file:
    // it is brought in once.
    std::map<juce::String, juce::File> broughtIn;   // source path -> file in the folder
    for (const auto& track : session.getTracks())
    {
        for (const auto& clip : track->clips)
        {
            if (clip.data == nullptr || clip.data->file == juce::File())
                continue;   // in-memory only (tests); nothing to reference

            const auto source = clip.data->file.getFullPathName();
            auto it = broughtIn.find (source);
            if (it == broughtIn.end())
            {
                auto moved = clip.data->file.existsAsFile() ? bringIntoFolder (clip.data->file, audioFolder, unsavedFolder)
                                                            : juce::File();
                if (moved == juce::File())   // gone from disk (or not copyable): save the audio in memory
                    moved = writeFromMemory (*clip.data, audioFolder, clip.data->file.getFileNameWithoutExtension());
                if (moved == juce::File())
                    return utf8 ("오디오 파일을 세션 폴더에 저장할 수 없습니다: ") + source;

                it = broughtIn.emplace (source, moved).first;
                broughtIn.emplace (moved.getFullPathName(), moved);   // already moved data
            }

            clip.data->file = it->second;
        }
    }

    // 2. The XML.
    juce::XmlElement root ("DAWSession");
    root.setAttribute ("version", 1);
    root.setAttribute ("sampleRate", session.getSampleRate());
    root.setAttribute ("tempo", session.getTempo());

    for (const auto& track : session.getTracks())
    {
        auto* t = root.createNewChildElement ("Track");
        t->setAttribute ("id", track->id);
        t->setAttribute ("name", track->name);
        t->setAttribute ("colour", track->colour.toString());
        t->setAttribute ("volumeDb", (double) track->volumeDb);
        t->setAttribute ("pan", (double) track->pan);
        t->setAttribute ("mute", track->mute);
        t->setAttribute ("solo", track->solo);
        t->setAttribute ("input", track->inputChannel);
        t->setAttribute ("stereoInput", track->stereoInput);

        for (const auto& clip : track->clips)
        {
            if (clip.data == nullptr || clip.data->file == juce::File())
                continue;

            auto* c = t->createNewChildElement ("Clip");
            c->setAttribute ("id", clip.id);
            c->setAttribute ("name", clip.name);
            const auto& f = clip.data->file;
            c->setAttribute ("file", f.isAChildOf (folder) ? f.getRelativePathFrom (folder).replace ("\\", "/")
                                                           : f.getFullPathName());
            c->setAttribute ("start", juce::String (clip.start));
            c->setAttribute ("offset", juce::String (clip.offset));
            c->setAttribute ("length", juce::String (clip.length));
            c->setAttribute ("gainDb", (double) clip.gainDb);
            c->setAttribute ("fadeIn", juce::String (clip.fadeIn));
            c->setAttribute ("fadeOut", juce::String (clip.fadeOut));
        }

        if (auto chain = track->getInserts().createStateXml())
            t->addChildElement (chain.release());
    }

    auto* masterXml = root.createNewChildElement ("Master");
    masterXml->setAttribute ("volumeDb", (double) session.getMasterVolume());
    if (auto chain = session.getMasterInserts().createStateXml())
        masterXml->addChildElement (chain.release());

    // Write next to the target first, so a failed write never destroys the old file.
    const auto temp = target.getSiblingFile (target.getFileName() + ".tmp");
    if (! root.writeTo (temp) || ! temp.moveFileTo (target))
    {
        temp.deleteFile();
        return utf8 ("세션 파일을 저장할 수 없습니다: ") + target.getFullPathName();
    }

    session.setFile (target);
    session.setDirty (false);
    return {};
}

bool load (Session& session, const juce::File& file, juce::String& error,
           std::function<void (juce::StringArray)> onDone)
{
    const auto xml = parse (file, error);
    if (xml == nullptr)
        return false;

    const auto folder = file.getParentDirectory();
    const double rate = xml->getDoubleAttribute ("sampleRate", 48000.0);

    session.reset (rate);
    session.setTempo (xml->getDoubleAttribute ("tempo", 120.0));

    struct Pending
    {
        int remaining = 1;   // released after all chains were started
        juce::StringArray problems;
        std::function<void (juce::StringArray)> onDone;

        void finishOne()
        {
            if (--remaining == 0 && onDone)
                onDone (problems);
        }
    };
    auto pending = std::make_shared<Pending>();
    pending->onDone = std::move (onDone);

    auto restoreChain = [pending] (PluginChain& chain, const juce::XmlElement* chainXml, const juce::String& where)
    {
        if (chainXml == nullptr || chainXml->getNumChildElements() == 0)
            return;

        ++pending->remaining;
        chain.restoreFromXml (*chainXml, [pending, where] (PluginChain::RestoreResult result)
        {
            for (const auto& name : result.failedNames)
                pending->problems.add (where + utf8 (": 플러그인을 불러오지 못했습니다 - ") + name);
            pending->finishOne();
        });
    };

    std::map<juce::String, AudioDataPtr> loaded;   // path -> data, so shared files load once
    Session::ClipMap clips;
    int maxClipId = 0;

    for (auto* t : xml->getChildWithTagNameIterator ("Track"))
    {
        auto* track = session.addTrack (t->getStringAttribute ("name"));
        if (track == nullptr)
            break;

        if (t->hasAttribute ("colour"))
            track->colour = juce::Colour::fromString (t->getStringAttribute ("colour"));

        session.setTrackVolume (track->id, (float) t->getDoubleAttribute ("volumeDb"));
        session.setTrackPan (track->id, (float) t->getDoubleAttribute ("pan"));
        session.setTrackMute (track->id, t->getBoolAttribute ("mute"));
        session.setTrackSolo (track->id, t->getBoolAttribute ("solo"));
        session.setTrackInput (track->id, t->getIntAttribute ("input"), t->getBoolAttribute ("stereoInput"));

        auto& list = clips[track->id];

        for (auto* c : t->getChildWithTagNameIterator ("Clip"))
        {
            const auto path = c->getStringAttribute ("file");
            const auto audioFile = juce::File::isAbsolutePath (path) ? juce::File (path) : folder.getChildFile (path);
            const auto key = audioFile.getFullPathName();

            if (loaded.count (key) == 0)
            {
                juce::String loadError;
                loaded[key] = loadAudioFile (session.getAudioFormats(), audioFile, rate, loadError);
                if (loaded[key] == nullptr)
                    pending->problems.add (loadError);
            }

            const auto data = loaded[key];
            if (data == nullptr)
                continue;

            Clip clip;
            clip.id = c->getIntAttribute ("id");
            clip.name = c->getStringAttribute ("name");
            clip.data = data;
            clip.start = c->getStringAttribute ("start").getLargeIntValue();
            clip.offset = juce::jlimit<juce::int64> (0, data->getLength() - 1, c->getStringAttribute ("offset").getLargeIntValue());
            clip.length = juce::jlimit<juce::int64> (1, data->getLength() - clip.offset, c->getStringAttribute ("length").getLargeIntValue());
            clip.gainDb = (float) c->getDoubleAttribute ("gainDb");
            clip.fadeIn = c->getStringAttribute ("fadeIn").getLargeIntValue();
            clip.fadeOut = c->getStringAttribute ("fadeOut").getLargeIntValue();
            ClipEdit::clampFades (clip);
            maxClipId = std::max (maxClipId, clip.id);
            list.push_back (clip);
        }

        restoreChain (track->getInserts(), t->getChildByName ("DAWChain"), track->name);
    }

    // Ids from the file stay unique: new ones start after the largest.
    while (session.newClipId() <= maxClipId) {}

    // Ids must be unique within the session even if the file was edited by
    // hand: missing and repeated ones get new ids.
    std::set<int> seen;
    for (auto& [id, list] : clips)
    {
        for (auto& clip : list)
        {
            if (clip.id <= 0 || seen.count (clip.id) != 0)
                clip.id = session.newClipId();
            seen.insert (clip.id);
        }
        ClipEdit::sort (list);
    }

    session.applyClips (clips);

    if (auto* m = xml->getChildByName ("Master"))
    {
        session.setMasterVolume ((float) m->getDoubleAttribute ("volumeDb"));
        restoreChain (session.getMasterInserts(), m->getChildByName ("DAWChain"), utf8 ("마스터"));
    }

    session.setFile (file);
    session.getUndoManager().clearUndoHistory();
    session.setDirty (false);
    pending->finishOne();
    return true;
}
} // namespace SessionFile
