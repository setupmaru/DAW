#include "PluginEditorWindow.h"

PluginEditorWindow::PluginEditorWindow (const juce::String& pluginName,
                                        std::unique_ptr<juce::AudioProcessorEditor> editor,
                                        std::function<void()> closeCallback)
    : DocumentWindow (pluginName,
                      juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::closeButton),
      onCloseRequested (std::move (closeCallback))
{
    jassert (editor != nullptr);
    const bool editorResizable = editor != nullptr && editor->isResizable();

    setUsingNativeTitleBar (true);
    setContentOwned (editor.release(), true);   // the window now owns the editor
    setResizable (editorResizable, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

PluginEditorWindow::~PluginEditorWindow()
{
    // Deletes the editor now, while the processor is guaranteed to exist.
    clearContentComponent();
}

void PluginEditorWindow::closeButtonPressed()
{
    // The owner may delete this window inside the callback: call a copy, and
    // touch no member afterwards.
    if (auto callback = onCloseRequested)
        callback();
    else
        setVisible (false);
}
