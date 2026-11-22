#include "Clip.h"

#include <algorithm>

namespace ClipEdit
{
void sort (std::vector<Clip>& clips)
{
    std::stable_sort (clips.begin(), clips.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
}

void clampFades (Clip& clip)
{
    clip.fadeIn = juce::jlimit<juce::int64> (0, clip.length, clip.fadeIn);
    clip.fadeOut = juce::jlimit<juce::int64> (0, clip.length - clip.fadeIn, clip.fadeOut);
}

void clearRange (std::vector<Clip>& clips, juce::int64 from, juce::int64 to, const IdSource& newId)
{
    if (to <= from)
        return;

    std::vector<Clip> result;
    result.reserve (clips.size() + 1);

    for (const auto& clip : clips)
    {
        if (clip.end() <= from || clip.start >= to)
        {
            result.push_back (clip);
            continue;
        }

        if (clip.start < from)   // keep the left part; its right edge is a cut
        {
            auto left = clip;
            left.length = from - clip.start;
            left.fadeOut = 0;
            clampFades (left);
            result.push_back (left);
        }

        if (clip.end() > to)     // keep the right part; its left edge is a cut
        {
            auto right = clip;
            if (clip.start < from)
                right.id = newId();   // the left part kept the id
            right.offset = clip.offset + (to - clip.start);
            right.start = to;
            right.length = clip.end() - to;
            right.fadeIn = 0;
            clampFades (right);
            result.push_back (right);
        }
    }

    clips = std::move (result);
    sort (clips);
}

void place (std::vector<Clip>& clips, Clip clip, const IdSource& newId)
{
    clampFades (clip);
    clearRange (clips, clip.start, clip.end(), newId);
    clips.push_back (std::move (clip));
    sort (clips);
}

int splitAt (std::vector<Clip>& clips, juce::int64 position, const IdSource& newId)
{
    for (size_t i = 0; i < clips.size(); ++i)
    {
        const auto clip = clips[i];

        if (position <= clip.start || position >= clip.end())
            continue;

        auto left = clip;
        left.length = position - clip.start;
        left.fadeOut = 0;
        clampFades (left);

        auto right = clip;
        right.id = newId();
        right.start = position;
        right.offset = clip.offset + left.length;
        right.length = clip.end() - position;
        right.fadeIn = 0;
        clampFades (right);

        clips[i] = left;
        clips.insert (clips.begin() + (std::ptrdiff_t) i + 1, right);
        return right.id;
    }

    return 0;
}

void trimStart (Clip& clip, juce::int64 newStart)
{
    const auto earliest = clip.start - clip.offset;   // source sample 0
    const auto latest = clip.end() - 1;
    newStart = juce::jlimit (earliest, latest, newStart);

    const auto delta = newStart - clip.start;
    clip.start = newStart;
    clip.offset += delta;
    clip.length -= delta;
    clampFades (clip);
}

void trimEnd (Clip& clip, juce::int64 newEnd)
{
    const auto latest = clip.start + (clip.sourceLength() - clip.offset);
    newEnd = juce::jlimit (clip.start + 1, std::max (clip.start + 1, latest), newEnd);
    clip.length = newEnd - clip.start;
    clampFades (clip);
}

int indexOf (const std::vector<Clip>& clips, int id)
{
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].id == id)
            return (int) i;

    return -1;
}
} // namespace ClipEdit
