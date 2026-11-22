#include <juce_gui_extra/juce_gui_extra.h>

#include "MainComponent.h"
#include "ui/DAWLookAndFeel.h"

class DAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "DAW"; }
    const juce::String getApplicationVersion() override { return DAW_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // Before any window exists, so every component (dialogs and popup
        // menus included) uses the dark theme and the Korean font.
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> ("DAW");
        mainWindow->getContent().handleCommandLine (juce::StringArray::fromTokens (commandLine, true));
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (mainWindow == nullptr)
        {
            quit();
            return;
        }

        mainWindow->getContent().requestQuit ([] { juce::JUCEApplication::getInstance()->quit(); });
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow != nullptr)
            mainWindow->getContent().handleCommandLine (juce::StringArray::fromTokens (commandLine, true));
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Desktop::getInstance().getDefaultLookAndFeel()
                                        .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new MainComponent();
            setContentOwned (content, true);
            // Shortcuts also work when no component inside has the focus.
            addKeyListener (content->getCommandManager().getKeyMappings());
            setResizable (true, true);
            setResizeLimits (960, 600, 8192, 8192);
            centreWithSize (1400, 860);
            setVisible (true);
            content->grabKeyboardFocus();
        }

        MainComponent& getContent() { return *content; }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        MainComponent* content = nullptr;   // owned by the window
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    // Declared before the window: it must outlive every component using it.
    DAWLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (DAWApplication)
