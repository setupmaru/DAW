#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <set>

// Edit tools and modes, as on a tape-style DAW:
//  - Trim: drag a clip's edge to shorten or lengthen it.
//  - Select: drag to select a time range across tracks; click to put the cursor.
//  - Grab: click to select clips, drag to move them (Alt: copy).
//  Slip mode moves freely; Grid mode snaps to the grid (bars / beats).
enum class EditTool { trim, select, grab };
enum class EditMode { slip, grid };

struct TimeSelection
{
    juce::int64 start = 0, end = 0;
    std::set<int> trackIds;

    bool isEmpty() const noexcept { return end <= start; }
    juce::int64 length() const noexcept { return end - start; }
};

// UI state shared by the edit window, the transport bar and the commands:
// zoom and scroll, tool and mode, the selections and the edit cursor.
// Sends a change message (asynchronously) when something changes.
//
// Threads: message thread only.
class EditState : public juce::ChangeBroadcaster
{
public:
    static constexpr int kHeaderWidth = 230;
    static constexpr int kRulerHeight = 44;
    static constexpr double kMinSamplesPerPixel = 1.0, kMaxSamplesPerPixel = 100000.0;

    // Grid divisions: whole bar, half, quarter, eighth, sixteenth (4/4).
    static juce::StringArray gridNames();
    static constexpr int kNumGrids = 5;

    double sampleRate = 48000.0;
    double tempo = 120.0;

    double samplesPerPixel = 600.0;
    double scrollSamples = 0.0;     // timeline position at the left edge
    int scrollY = 0;                // vertical scroll of the track area
    int trackHeight = 96;

    EditTool tool = EditTool::grab;
    EditMode mode = EditMode::slip;
    int gridIndex = 2;              // 1/4

    std::set<int> selectedClips;
    TimeSelection selection;
    juce::int64 cursor = 0;         // edit cursor (insert point)
    int selectedTrack = 0;          // 0: none
    bool showBars = true;           // main counter in bars|beats (else min:sec)

    float timeToX (juce::int64 t) const noexcept  { return (float) (((double) t - scrollSamples) / samplesPerPixel); }
    juce::int64 xToTime (float x) const noexcept  { return (juce::int64) std::llround (scrollSamples + (double) x * samplesPerPixel); }

    double beatSamples() const noexcept { return sampleRate * 60.0 / tempo; }
    double barSamples() const noexcept  { return beatSamples() * 4.0; }
    double gridSamples() const noexcept;
    // In grid mode: the nearest grid line; in slip mode: t itself.
    juce::int64 snap (juce::int64 t) const noexcept;

    // Zooms keeping the timeline position under x (pixels from the left) fixed.
    void zoom (double factor, float anchorX);
    void setScroll (double samples);

    void clearSelection();   // clips and time range
    void changed() { sendChangeMessage(); }

    juce::String formatMinSec (juce::int64 t) const;   // 1:23.456
    juce::String formatBars (juce::int64 t) const;     // 12|3|240 (bar|beat|tick of 960)
    juce::String formatMain (juce::int64 t) const { return showBars ? formatBars (t) : formatMinSec (t); }
};
