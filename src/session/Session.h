#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "engine/Clip.h"
#include "engine/MixEngine.h"
#include "plugins/PluginChain.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

//==============================================================================
// One audio track: its clips and mixer settings (the model), its audio-side
// object (fader state, meters, insert chain) and the insert chain's model.
//
// Fields are edited through Session, which pushes mixer settings to the
// TrackAudio and republishes the clips for playback.
class Track
{
public:
    Track (int id, MixEngine&, juce::AudioPluginFormatManager&, PluginChain::ProcessorFactory = {});

    const int id;
    juce::String name;
    juce::Colour colour;
    std::vector<Clip> clips;   // sorted, not overlapping (ClipEdit rules)

    float volumeDb = 0.0f;
    float pan = 0.0f;          // -1 (left) .. 1 (right)
    bool mute = false, solo = false, armed = false;
    bool inputMonitor = false; // "I": always hear the input instead of the clips
    int inputChannel = 0;      // device input (0-based); with stereoInput, also inputChannel + 1
    bool stereoInput = false;

    TrackAudio& getAudio() noexcept { return *audio; }
    const std::shared_ptr<TrackAudio>& getAudioPtr() const noexcept { return audio; }
    PluginChain& getInserts() noexcept { return *inserts; }

    void pushParameters();     // copies the mixer fields to the TrackAudio

private:
    // Declaration order: the PluginChain refers to the TrackAudio's chain and
    // must be destroyed first.
    std::shared_ptr<TrackAudio> audio;
    std::unique_ptr<PluginChain> inserts;

    JUCE_DECLARE_NON_COPYABLE (Track)
};

//==============================================================================
// The open session: tracks, clips, master bus, tempo, file and undo history.
// Every change republishes a playback snapshot to the MixEngine and sends a
// change message (asynchronously) to the UI.
//
// Clip edits are undoable as whole before/after clip lists per track. Track
// creation/removal and mixer moves are not part of the undo history.
//
// Threads: message thread only.
class Session : public juce::ChangeBroadcaster,
                private juce::Timer
{
public:
    static constexpr int kMaxTracks = 64;
    using ClipMap = std::map<int, std::vector<Clip>>;   // track id -> clips

    Session (MixEngine&, juce::AudioPluginFormatManager& pluginFormats, juce::AudioFormatManager& audioFormats,
             PluginChain::ProcessorFactory = {});
    ~Session() override;

    MixEngine& getEngine() noexcept { return engine; }
    juce::AudioFormatManager& getAudioFormats() noexcept { return audioFormats; }
    juce::AudioPluginFormatManager& getPluginFormats() noexcept { return pluginFormats; }

    // Removes every track and clip, clears the master inserts and the undo
    // history, forgets the file. The sample rate is set to `sampleRate`.
    // Track and clip ids are not reused afterwards.
    void reset (double sampleRate);

    double getSampleRate() const noexcept { return sampleRate; }
    double getTempo() const noexcept { return tempo; }
    void setTempo (double bpm);

    juce::File getFile() const { return file; }
    void setFile (const juce::File& f);
    juce::String getName() const;   // file name, or "제목 없음"
    bool isDirty() const noexcept { return dirty; }
    void setDirty (bool);

    // <session folder>/Audio Files, or a recording folder in the user's
    // application data while the session has never been saved.
    juce::File getAudioFolder() const;

    //--------------------------------------------------------------------------
    const std::vector<std::unique_ptr<Track>>& getTracks() const noexcept { return tracks; }
    Track* findTrack (int id) const;
    int indexOfTrack (int id) const;

    Track* addTrack (const juce::String& name = {});   // nullptr when kMaxTracks exist
    void removeTrack (int id);   // call onTrackRemoving first (editor windows)
    void moveTrack (int id, int newIndex);
    void renameTrack (int id, const juce::String&);
    void setTrackVolume (int id, float db);
    void setTrackPan (int id, float pan);
    void setTrackMute (int id, bool);
    void setTrackSolo (int id, bool);
    void setTrackArmed (int id, bool);
    void setTrackInputMonitor (int id, bool);
    void setTrackInput (int id, int firstChannel, bool stereo);

    PluginChain& getMasterInserts() noexcept { return *masterInserts; }
    float getMasterVolume() const noexcept { return masterVolumeDb; }
    void setMasterVolume (float db);

    //--------------------------------------------------------------------------
    // Clips.
    int newClipId() { return nextClipId++; }
    ClipEdit::IdSource idSource() { return [this] { return newClipId(); }; }
    const Clip* findClip (int clipId, int* trackId = nullptr) const;

    ClipMap captureClips() const;
    void applyClips (const ClipMap&);   // no undo entry (drags, undo itself)
    // Records the change from `before` to the current clips as one undo step.
    void commitClipEdit (const juce::String& actionName, const ClipMap& before);
    // Copies the clips, lets `edit` change the copy, applies and commits it.
    void editClips (const juce::String& actionName, const std::function<void (ClipMap&)>& edit);

    // Loads a file and places it on a track (overwriting what is under it).
    // Returns the new clip id, or 0 with a Korean message in `error`.
    int importAudioFile (const juce::File&, int trackId, juce::int64 position, juce::String& error);

    void splitClips (const std::vector<int>& trackIds, juce::int64 position);
    void deleteClips (const std::set<int>& clipIds);
    void deleteRange (const std::vector<int>& trackIds, juce::int64 from, juce::int64 to);
    std::set<int> duplicateClips (const std::set<int>& clipIds);   // placed right after each; new ids

    juce::int64 getLength() const;   // end of the last clip (0 when empty)
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    //--------------------------------------------------------------------------
    // Recording: every armed track records its input into a new WAV file from
    // the current position. Returns "" or a Korean error. Finished takes
    // become clips (one undo step "녹음") shortly after the transport stops.
    juce::String startRecording();

    // Rebuilds the playback snapshot from the clips (done after every change).
    void publish();

    std::function<void (const juce::String&)> onError;     // Korean, user-facing
    std::function<void (int trackId)> onTrackRemoving;     // before a track is deleted
    // Synchronously, whenever an insert chain's entries changed. Editor
    // windows of removed plugins must close here, before the processor can
    // be deleted.
    std::function<void()> onInsertsChanged;

    // Takes the device's rate when the session has no audio yet (returns
    // true then); a session with clips keeps its rate.
    bool adoptSampleRate (double rate);

    // While true (a mouse drag of clips), finished takes are not turned into
    // clips yet: the drag would overwrite them.
    void setGestureActive (bool active) { gestureActive = active; }

    // Tests: handles finished recordings and garbage without waiting for the timer.
    void pumpForTest() { timerCallback(); }

private:
    void timerCallback() override;
    void collectRecording (RecordJob&);
    void changed();
    void attachChain (PluginChain&);
    juce::Colour nextColour();

    MixEngine& engine;
    juce::AudioPluginFormatManager& pluginFormats;
    juce::AudioFormatManager& audioFormats;
    PluginChain::ProcessorFactory processorFactory;

    std::vector<std::unique_ptr<Track>> tracks;
    std::unique_ptr<PluginChain> masterInserts;
    float masterVolumeDb = 0.0f;

    double sampleRate = 48000.0;
    double tempo = 120.0;
    juce::File file;
    bool dirty = false;
    int nextTrackId = 1, nextClipId = 1, colourIndex = 0;

    juce::UndoManager undoManager;
    bool gestureActive = false;

    JUCE_DECLARE_NON_COPYABLE (Session)
};
