#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

// A top-level window showing one plugin's editor. Its title is the plugin name.
//
// The window owns the editor. Deleting the window deletes the editor, which
// tells its processor (AudioProcessor::editorBeingDeleted), so the window must
// be deleted before the processor. MainComponent keeps these windows in an
// id -> window map and deletes a window as soon as its entry leaves the chain.
//
// The close button does not delete anything by itself: it calls
// onCloseRequested, and the owner deletes the window (the plugin stays in the
// chain). The window may be deleted inside that call.
//
// Threads: message thread only.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow (const juce::String& pluginName,
                        std::unique_ptr<juce::AudioProcessorEditor> editor,
                        std::function<void()> onCloseRequested);
    ~PluginEditorWindow() override;

    void closeButtonPressed() override;

private:
    std::function<void()> onCloseRequested;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditorWindow)
};
