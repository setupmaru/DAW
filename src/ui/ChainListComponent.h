#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

// The effect rows ("1. 이름  [켜짐] [편집] [↑] [↓] [삭제]") followed by the
// "+ 플러그인 추가" button, in a vertically scrolling viewport.
//
// It only shows a snapshot (RowData) and reports clicks by entry id; it never
// touches the chain or a processor itself. setRows() rebuilds the rows
// asynchronously: a click handler usually changes the chain, the chain calls
// onChanged synchronously, and rebuilding at once would delete the button
// whose click is still being handled.
//
// Threads: message thread only.
class ChainListComponent : public juce::Component,
                           private juce::AsyncUpdater
{
public:
    struct RowData
    {
        int id;
        juce::String name;
        bool bypassed;
        bool hasEditor;
    };

    ChainListComponent();
    ~ChainListComponent() override;

    // Stores the snapshot and rebuilds the rows on the next message loop pass.
    void setRows (std::vector<RowData>);

    // For anchoring the add-plugin popup menu.
    juce::Component& getAddButton() { return addButton; }

    std::function<void (int id, bool bypassed)> onBypassRequested;   // the requested new state
    std::function<void (int id)> onEditRequested;
    std::function<void (int id, int newIndex)> onMoveRequested;
    std::function<void (int id)> onRemoveRequested;
    std::function<void()> onAddRequested;

    void resized() override;

private:
    class Row;

    void handleAsyncUpdate() override;
    void layoutContent();

    std::vector<RowData> rowData;
    juce::Viewport viewport;
    juce::Component content;   // shown in the viewport, not owned by it
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label emptyHint;
    juce::TextButton addButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainListComponent)
};
