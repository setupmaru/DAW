#pragma once

#include "AudioData.h"

#include <functional>
#include <vector>

// One clip on a track's timeline: a window [offset, offset + length) into its
// audio data, placed at `start`. All positions are in session samples.
//
// A value type: the whole clip list of a track is copied for undo and for the
// playback snapshot (the audio itself is shared, never copied).
struct Clip
{
    int id = 0;
    juce::String name;
    AudioDataPtr data;
    juce::int64 start = 0;     // timeline position of the first sample
    juce::int64 offset = 0;    // first sample used from `data`
    juce::int64 length = 0;
    float gainDb = 0.0f;
    juce::int64 fadeIn = 0;    // linear fade lengths in samples, inside the clip
    juce::int64 fadeOut = 0;

    juce::int64 end() const noexcept { return start + length; }
    juce::int64 sourceLength() const noexcept { return data != nullptr ? data->getLength() : 0; }

    bool operator== (const Clip& o) const noexcept
    {
        return id == o.id && name == o.name && data == o.data && start == o.start && offset == o.offset
            && length == o.length && juce::exactlyEqual (gainDb, o.gainDb) && fadeIn == o.fadeIn && fadeOut == o.fadeOut;
    }
    bool operator!= (const Clip& o) const noexcept { return ! (*this == o); }
};

// Pure clip-list editing. A track holds at most one clip at any timeline
// position (as on a tape track): placing a clip overwrites what was under it.
// Lists are kept sorted by start. Functions that create clips take an id
// source so the caller keeps ids unique.
namespace ClipEdit
{
    using IdSource = std::function<int()>;

    void sort (std::vector<Clip>&);

    // Keeps fades inside the clip (fadeIn + fadeOut <= length, each >= 0).
    void clampFades (Clip&);

    // Removes the audio in [from, to) from the list, shortening or splitting
    // the clips that cross the range. A split clip keeps its id on the left
    // part; the right part gets a new id.
    void clearRange (std::vector<Clip>&, juce::int64 from, juce::int64 to, const IdSource&);

    // clearRange over the clip's extent, then inserts it.
    void place (std::vector<Clip>&, Clip clip, const IdSource&);

    // Splits the clip that contains `position` (strictly inside it).
    // Returns the new right part's id, or 0 when no clip was split.
    int splitAt (std::vector<Clip>&, juce::int64 position, const IdSource&);

    // Moves the clip's left edge to `newStart`, keeping the audio in place
    // (the source offset follows). Clamped to the source and to 1 sample.
    void trimStart (Clip&, juce::int64 newStart);

    // Moves the clip's right edge to `newEnd`, clamped to the source and to 1 sample.
    void trimEnd (Clip&, juce::int64 newEnd);

    // Index of the clip with `id`, or -1.
    int indexOf (const std::vector<Clip>&, int id);
}
