#pragma once

#include "Session.h"

#include <functional>

// Reading and writing session files (*.dawsession, XML):
//
//   <DAWSession version="1" sampleRate="48000" tempo="120">
//     <Track id name colour volumeDb pan mute solo inputMonitor input stereoInput>
//       <Clip id name file start offset length gainDb fadeIn fadeOut/>
//       <DAWChain .../>            the insert chain (PluginChain format)
//     </Track>
//     <Master volumeDb><DAWChain/></Master>
//   </DAWSession>
//
// Audio lives in "<session folder>/Audio Files". Saving copies (or, for
// unsaved recordings, moves) every clip's file there first, so a session
// folder is self-contained and can be moved as a whole. Clip paths are stored
// relative to the session folder when they are inside it.
//
// Threads: message thread only.
namespace SessionFile
{
    inline constexpr const char* kExtension = ".dawsession";

    // Returns "" on success or a Korean error message. On success the session
    // takes the file as its own and is no longer dirty.
    juce::String save (Session&, const juce::File&);

    // Replaces the session's content with the file's. Audio loads at once;
    // insert plugins load asynchronously, and onDone is called when every
    // chain has finished, with a list of problems (missing files, plugins
    // that failed) in Korean. Returns false (with the reason in `error`, the
    // session unchanged) when the file is not a readable session.
    bool load (Session&, const juce::File&, juce::String& error,
               std::function<void (juce::StringArray problems)> onDone);

    // The sample rate stored in a session file, or 0 if it cannot be read.
    double readSampleRate (const juce::File&);
}
