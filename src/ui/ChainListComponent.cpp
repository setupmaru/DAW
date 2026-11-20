#include "ChainListComponent.h"

#include <cmath>

namespace
{
juce::String utf8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

constexpr int kRowHeight = 36;
constexpr int kAddButtonHeight = 32;
constexpr int kHintHeight = 28;
constexpr int kMargin = 8;
} // namespace

juce::String ui::formatMs (double ms)
{
    // "0.8ms" below 10 ms, "12ms" above: enough precision for a beginner.
    // Round first, so 9.96 becomes "10ms" rather than "10.0ms".
    const double tenths = std::round (ms * 10.0) / 10.0;

    if (tenths < 10.0)
        return juce::String (tenths, 1) + "ms";

    return juce::String (juce::roundToInt (ms)) + "ms";
}

//==============================================================================
// One effect row. Buttons report through the owner's callbacks using the id and
// the index from the snapshot the row was built from.
class ChainListComponent::Row : public juce::Component
{
public:
    Row (ChainListComponent& o, const RowData& d, int idx, int count)
        : owner (o), data (d), index (idx)
    {
        const bool hasLatency = data.latencyMs >= ui::kMinShownLatencyMs;
        auto title = juce::String (index + 1) + ". " + data.name;

        if (hasLatency)
            title << utf8 ("  ·  지연 ") << ui::formatMs (data.latencyMs);

        nameLabel.setText (title, juce::dontSendNotification);
        nameLabel.setMinimumHorizontalScale (0.7f);

        // The full title always, since a long name can be squeezed or cut.
        auto tooltip = title;

        if (hasLatency)
            tooltip << "\n" << (data.bypassed ? utf8 ("이 플러그인은 꺼 둬도 소리를 조금 늦춥니다. ")
                                              : utf8 ("이 플러그인은 소리를 조금 늦춥니다. "))
                    << utf8 ("넣거나 뺄 때 소리가 아주 잠깐 비는 것은 정상입니다.");

        nameLabel.setTooltip (tooltip);
        // An effect that is off stays in the chain but no longer changes the sound.
        nameLabel.setAlpha (data.bypassed ? 0.45f : 1.0f);
        addAndMakeVisible (nameLabel);

        // Shows the model state; clicking asks for the opposite state. The
        // button does not toggle itself, so a refused change leaves it correct.
        bypassButton.setButtonText (data.bypassed ? utf8 ("꺼짐") : utf8 ("켜짐"));
        bypassButton.setToggleState (! data.bypassed, juce::dontSendNotification);
        bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::seagreen);
        bypassButton.setTooltip (data.bypassed ? utf8 ("눌러서 이 효과를 다시 켭니다.")
                                               : utf8 ("눌러서 이 효과를 끕니다. 마이크 소리는 계속 나갑니다."));
        bypassButton.onClick = [this] { call (owner.onBypassRequested, data.id, ! data.bypassed); };
        addAndMakeVisible (bypassButton);

        editButton.setButtonText (utf8 ("편집"));
        editButton.setEnabled (data.hasEditor);   // no editor: disabled
        editButton.setTooltip (data.hasEditor ? utf8 ("플러그인 설정 창을 엽니다.")
                                              : utf8 ("이 플러그인은 설정 창이 없습니다."));
        editButton.onClick = [this] { call (owner.onEditRequested, data.id); };
        addAndMakeVisible (editButton);

        upButton.setButtonText (utf8 ("↑"));
        upButton.setEnabled (index > 0);
        upButton.setTooltip (utf8 ("위로 옮깁니다. 위에 있는 효과부터 차례로 적용됩니다."));
        upButton.onClick = [this] { call (owner.onMoveRequested, data.id, index - 1); };
        addAndMakeVisible (upButton);

        downButton.setButtonText (utf8 ("↓"));
        downButton.setEnabled (index < count - 1);
        downButton.setTooltip (utf8 ("아래로 옮깁니다. 위에 있는 효과부터 차례로 적용됩니다."));
        downButton.onClick = [this] { call (owner.onMoveRequested, data.id, index + 1); };
        addAndMakeVisible (downButton);

        removeButton.setButtonText (utf8 ("삭제"));
        removeButton.setTooltip (utf8 ("이 효과를 체인에서 뺍니다. 소리가 튀지 않게 부드럽게 빠집니다."));
        removeButton.onClick = [this] { call (owner.onRemoveRequested, data.id); };
        addAndMakeVisible (removeButton);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (0, 3);

        removeButton.setBounds (area.removeFromRight (56));
        area.removeFromRight (10);
        downButton.setBounds (area.removeFromRight (34));
        area.removeFromRight (2);
        upButton.setBounds (area.removeFromRight (34));
        area.removeFromRight (10);
        editButton.setBounds (area.removeFromRight (56));
        area.removeFromRight (4);
        bypassButton.setBounds (area.removeFromRight (56));
        area.removeFromRight (8);
        nameLabel.setBounds (area);
    }

private:
    // Copies the callback before calling it, so it stays valid even if the
    // owner's member is reassigned during the call.
    template <typename Fn, typename... Args>
    static void call (const Fn& fn, Args... args)
    {
        if (auto copy = fn)
            copy (args...);
    }

    ChainListComponent& owner;
    const RowData data;
    const int index;
    juce::Label nameLabel;
    juce::TextButton bypassButton, editButton, upButton, downButton, removeButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
};

//==============================================================================
ChainListComponent::ChainListComponent()
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    emptyHint.setText (utf8 ("추가한 플러그인이 없습니다. 지금은 마이크 소리가 그대로 나갑니다."),
                       juce::dontSendNotification);
    emptyHint.setMinimumHorizontalScale (0.7f);
    content.addAndMakeVisible (emptyHint);

    addButton.setButtonText (utf8 ("+ 플러그인 추가"));
    addButton.setTooltip (utf8 ("스캔해 둔 VST3 플러그인 중 하나를 체인 맨 아래에 넣습니다."));
    addButton.onClick = [this]
    {
        if (auto copy = onAddRequested)
            copy();
    };
    content.addAndMakeVisible (addButton);

    layoutContent();
}

ChainListComponent::~ChainListComponent()
{
    cancelPendingUpdate();
    rows.clear();
    viewport.setViewedComponent (nullptr, false);
}

void ChainListComponent::setRows (std::vector<RowData> newRows)
{
    rowData = std::move (newRows);
    triggerAsyncUpdate();
}

void ChainListComponent::setEditingEnabled (bool enabled)
{
    if (enabled == editingEnabled)
        return;

    editingEnabled = enabled;
    addButton.setEnabled (enabled);

    // Row buttons keep their own enabled state (e.g. ↑ on the first row); the
    // row as a whole is disabled on top of that.
    for (auto& row : rows)
        row->setEnabled (enabled);
}

void ChainListComponent::handleAsyncUpdate()
{
    rows.clear();   // removes them from `content` as they are deleted

    const int count = (int) rowData.size();

    for (int i = 0; i < count; ++i)
    {
        rows.push_back (std::make_unique<Row> (*this, rowData[(size_t) i], i, count));
        rows.back()->setEnabled (editingEnabled);
        content.addAndMakeVisible (*rows.back());
    }

    emptyHint.setVisible (rows.empty());
    layoutContent();
}

void ChainListComponent::resized()
{
    viewport.setBounds (getLocalBounds());
    layoutContent();
}

void ChainListComponent::layoutContent()
{
    // Always leave room for the scroll bar, so the width does not change when
    // the bar appears or disappears.
    const int width = juce::jmax (0, viewport.getWidth() - viewport.getScrollBarThickness());
    int y = kMargin;

    for (auto& row : rows)
    {
        row->setBounds (kMargin, y, juce::jmax (0, width - 2 * kMargin), kRowHeight);
        y += kRowHeight;
    }

    if (rows.empty())
    {
        emptyHint.setBounds (kMargin, y, juce::jmax (0, width - 2 * kMargin), kHintHeight);
        y += kHintHeight;
    }

    y += kMargin;
    addButton.setBounds (kMargin, y, 150, kAddButtonHeight);
    y += kAddButtonHeight + kMargin;

    content.setSize (width, y);
}
