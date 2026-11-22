#include "TimelineView.h"

#include "DAWLookAndFeel.h"

#include <cmath>
#include <limits>

namespace
{
juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

constexpr int kEdgeZone = 6;        // pixels at a clip edge that trim
constexpr int kHandleSize = 8;      // fade handle squares
constexpr int kClipHeader = 15;     // name strip

bool isAudioFile (const juce::String& path)
{
    return juce::File (path).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;wave");
}
} // namespace

TimelineView::TimelineView (Session& s, EditState& e, AppActions& a)
    : session (s), state (e), actions (a)
{
    setOpaque (true);
    // Takes focus when clicked, so shortcuts reach the command manager
    // (unhandled keys travel up to the main component's key listener).
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    session.addChangeListener (this);
    state.addChangeListener (this);
    startTimerHz (30);
}

TimelineView::~TimelineView()
{
    stopTimer();
    session.setGestureActive (false);
    state.removeChangeListener (this);
    session.removeChangeListener (this);
}

void TimelineView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

void TimelineView::timerCallback()
{
    auto& engine = session.getEngine();
    const auto playhead = engine.getPosition();

    if (engine.isPlaying())
    {
        // Follow the playhead page by page.
        const float x = state.timeToX (playhead);
        if (drag.kind == DragKind::none && (x > (float) getWidth() * 0.92f || x < 0.0f))
        {
            state.setScroll ((double) playhead - (double) getWidth() * 0.08 * state.samplesPerPixel);
            if (onScrollChanged)
                onScrollChanged();
        }
    }

    if (playhead != lastPlayhead)
    {
        const float oldX = state.timeToX (lastPlayhead), newX = state.timeToX (playhead);
        lastPlayhead = playhead;
        repaint (juce::Rectangle<float> (std::min (oldX, newX) - 2.0f, 0.0f, std::abs (newX - oldX) + 4.0f, (float) getHeight()).toNearestInt());
    }
}

int TimelineView::laneTop (int trackIndex) const
{
    return EditState::kRulerHeight + trackIndex * state.trackHeight - state.scrollY;
}

int TimelineView::trackIndexAt (int y) const
{
    if (y < EditState::kRulerHeight)
        return -1;

    const int index = (y - EditState::kRulerHeight + state.scrollY) / state.trackHeight;
    return index < (int) session.getTracks().size() ? index : -1;
}

int TimelineView::getLanesHeight() const
{
    return (int) session.getTracks().size() * state.trackHeight;
}

juce::Rectangle<float> TimelineView::clipBounds (const Clip& clip, int trackIndex) const
{
    const float x1 = state.timeToX (clip.start), x2 = state.timeToX (clip.end());
    return { x1, (float) laneTop (trackIndex) + 2.0f, std::max (2.0f, x2 - x1), (float) state.trackHeight - 5.0f };
}

TimelineView::Hit TimelineView::hitClip (juce::Point<int> p) const
{
    Hit hit;
    hit.trackIndex = trackIndexAt (p.y);
    if (hit.trackIndex < 0)
        return hit;

    const auto& track = *session.getTracks()[(size_t) hit.trackIndex];
    for (const auto& clip : track.clips)
    {
        const auto r = clipBounds (clip, hit.trackIndex);
        if (! r.contains (p.toFloat()) && ! r.expanded (2.0f, 0.0f).contains (p.toFloat()))
            continue;

        hit.clipId = clip.id;
        const float x = (float) p.x;
        const bool wide = r.getWidth() > 3.0f * kHandleSize;
        const bool top = (float) p.y < r.getY() + kHandleSize + 2.0f;

        if (state.tool == EditTool::trim)
            hit.zone = x < r.getCentreX() ? Zone::trimStart : Zone::trimEnd;
        else if (wide && top && x < r.getX() + kHandleSize + 2.0f)
            hit.zone = Zone::fadeIn;
        else if (wide && top && x > r.getRight() - kHandleSize - 2.0f)
            hit.zone = Zone::fadeOut;
        else if (x < r.getX() + kEdgeZone && r.getWidth() > 2.0f * kEdgeZone)
            hit.zone = Zone::trimStart;
        else if (x > r.getRight() - kEdgeZone && r.getWidth() > 2.0f * kEdgeZone)
            hit.zone = Zone::trimEnd;
        else
            hit.zone = Zone::body;
        return hit;
    }

    return hit;
}

//==============================================================================
void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (DAWColours::background);
    const auto lanes = getLocalBounds().withTrimmedTop (EditState::kRulerHeight);
    const auto& tracks = session.getTracks();

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (lanes);

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const int top = laneTop ((int) i);
            if (top > getHeight() || top + state.trackHeight < EditState::kRulerHeight)
                continue;

            const bool selectedTrack = tracks[i]->id == state.selectedTrack;
            g.setColour ((i % 2 == 0 ? DAWColours::laneA : DAWColours::laneB).brighter (selectedTrack ? 0.06f : 0.0f));
            g.fillRect (0, top, getWidth(), state.trackHeight);
            g.setColour (DAWColours::outline);
            g.drawHorizontalLine (top + state.trackHeight - 1, 0.0f, (float) getWidth());
        }

        paintGrid (g, lanes);

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const int top = laneTop ((int) i);
            if (top > getHeight() || top + state.trackHeight < EditState::kRulerHeight)
                continue;

            for (const auto& clip : tracks[i]->clips)
            {
                const auto r = clipBounds (clip, (int) i);
                if (r.getRight() < 0.0f || r.getX() > (float) getWidth())
                    continue;
                paintClip (g, clip, *tracks[i], (int) i);
            }
        }

        // Time selection over its tracks.
        if (! state.selection.isEmpty())
        {
            const float x1 = state.timeToX (state.selection.start), x2 = state.timeToX (state.selection.end);
            g.setColour (DAWColours::selection);
            for (size_t i = 0; i < tracks.size(); ++i)
                if (state.selection.trackIds.count (tracks[i]->id) != 0)
                    g.fillRect (juce::Rectangle<float> (x1, (float) laneTop ((int) i), x2 - x1, (float) state.trackHeight));
        }

        // Edit cursor.
        const float cx = state.timeToX (state.cursor);
        g.setColour (juce::Colour (0xff9ec9ff).withAlpha (0.8f));
        for (int y = EditState::kRulerHeight; y < getHeight(); y += 6)
            g.drawVerticalLine ((int) cx, (float) y, (float) y + 3.0f);

        if (tracks.empty())
        {
            g.setColour (DAWColours::textDim);
            g.setFont (juce::FontOptions (16.0f));
            g.drawFittedText (utf8 ("트랙이 없습니다.\n[트랙] → [새 트랙] (Ctrl+Shift+N)으로 만들거나, 오디오 파일을 여기에 끌어다 놓으세요."),
                              lanes.reduced (40), juce::Justification::centred, 3);
        }
    }

    paintRuler (g);

    // Playhead over everything.
    const float px = state.timeToX (session.getEngine().getPosition());
    g.setColour (session.getEngine().isRecording() ? DAWColours::record : DAWColours::playhead);
    g.fillRect (px - 0.5f, 0.0f, 1.5f, (float) getHeight());
    juce::Path head;
    head.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 9.0f);
    g.fillPath (head);
}

void TimelineView::paintGrid (juce::Graphics& g, juce::Rectangle<int> lanes)
{
    const double bar = state.barSamples(), beat = state.beatSamples();
    const double start = state.scrollSamples, end = state.scrollSamples + getWidth() * state.samplesPerPixel;
    const bool drawBeats = beat / state.samplesPerPixel >= 10.0;
    const double step = drawBeats ? beat : bar * std::pow (2.0, std::ceil (std::log2 (std::max (1.0, 12.0 * state.samplesPerPixel / bar))));

    for (double t = std::floor (start / step) * step; t < end; t += step)
    {
        const bool isBar = std::fmod (t + 0.5, bar) < 1.0;
        g.setColour (isBar ? DAWColours::gridBar : DAWColours::grid);
        g.drawVerticalLine ((int) state.timeToX ((juce::int64) t), (float) lanes.getY(), (float) lanes.getBottom());
    }
}

void TimelineView::paintClip (juce::Graphics& g, const Clip& clip, const Track& track, int trackIndex)
{
    const auto r = clipBounds (clip, trackIndex);
    const bool selected = state.selectedClips.count (clip.id) != 0;
    const auto colour = track.colour;

    g.setColour (colour.withMultipliedBrightness (0.45f).withMultipliedSaturation (0.8f));
    g.fillRoundedRectangle (r, 3.0f);

    auto header = r.withHeight ((float) kClipHeader);
    g.setColour (colour.withMultipliedBrightness (selected ? 1.05f : 0.75f));
    g.fillRoundedRectangle (header, 3.0f);
    g.fillRect (header.withTrimmedTop (3.0f));

    if (r.getWidth() > 20.0f)
    {
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (clip.name, header.withTrimmedLeft ((float) kHandleSize + 5.0f).withTrimmedRight ((float) kHandleSize + 3.0f).toNearestInt(),
                    juce::Justification::centredLeft, true);
    }

    // Waveform, pixel by pixel, from the overview.
    if (clip.data != nullptr)
    {
        juce::Graphics::ScopedSaveState save (g);
        auto wave = r.withTrimmedTop ((float) kClipHeader + 1.0f).reduced (0.0f, 2.0f);
        g.reduceClipRegion (wave.toNearestInt());
        const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
        const float gain = juce::Decibels::decibelsToGain (clip.gainDb);
        g.setColour (colour.brighter (0.55f).withAlpha (0.9f));

        const int x1 = std::max (0, (int) r.getX()), x2 = std::min (getWidth(), (int) r.getRight());
        for (int x = x1; x < x2; ++x)
        {
            const auto t0 = state.xToTime ((float) x) - clip.start + clip.offset;
            const auto t1 = state.xToTime ((float) x + 1.0f) - clip.start + clip.offset;
            const auto range = clip.data->getRange (t0, std::max (t1, t0 + 1));
            const float lo = juce::jlimit (-1.0f, 1.0f, range.first * gain), hi = juce::jlimit (-1.0f, 1.0f, range.second * gain);
            g.drawVerticalLine (x, mid - hi * half, mid - lo * half + 1.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.drawHorizontalLine ((int) mid, r.getX(), r.getRight());
    }

    // Fades: darken the faded part and draw the gain line.
    const auto body = r.withTrimmedTop ((float) kClipHeader);
    auto drawFade = [&] (juce::int64 from, juce::int64 to, bool fadeIn)
    {
        const float fx1 = state.timeToX (from), fx2 = state.timeToX (to);
        juce::Path shade;
        if (fadeIn)
            shade.addTriangle (fx1, body.getY(), fx2, body.getY(), fx1, body.getBottom());
        else
            shade.addTriangle (fx1, body.getY(), fx2, body.getY(), fx2, body.getBottom());
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillPath (shade);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        if (fadeIn)
            g.drawLine (fx1, body.getBottom(), fx2, body.getY(), 1.2f);
        else
            g.drawLine (fx1, body.getY(), fx2, body.getBottom(), 1.2f);
    };
    if (clip.fadeIn > 0)
        drawFade (clip.start, clip.start + clip.fadeIn, true);
    if (clip.fadeOut > 0)
        drawFade (clip.end() - clip.fadeOut, clip.end(), false);

    if (r.getWidth() > 3.0f * kHandleSize)
    {
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.drawRect (juce::Rectangle<float> (r.getX() + 1.0f, r.getY() + 1.0f, (float) kHandleSize, (float) kHandleSize), 1.0f);
        g.drawRect (juce::Rectangle<float> (r.getRight() - kHandleSize - 1.0f, r.getY() + 1.0f, (float) kHandleSize, (float) kHandleSize), 1.0f);
    }

    g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 3.0f, selected ? 2.0f : 1.0f);
}

void TimelineView::paintRuler (juce::Graphics& g)
{
    auto ruler = getLocalBounds().removeFromTop (EditState::kRulerHeight);
    g.setColour (DAWColours::panel);
    g.fillRect (ruler);
    g.setColour (DAWColours::outline);
    g.drawHorizontalLine (ruler.getBottom() - 1, 0.0f, (float) getWidth());
    g.drawHorizontalLine (ruler.getCentreY(), 0.0f, (float) getWidth());

    // Loop range.
    auto& engine = session.getEngine();
    if (engine.isLoopEnabled() && ! state.selection.isEmpty())
    {
        const float x1 = state.timeToX (state.selection.start), x2 = state.timeToX (state.selection.end);
        g.setColour (DAWColours::accent.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> (x1, 1.0f, x2 - x1, (float) ruler.getHeight() / 2.0f - 2.0f));
    }

    g.setFont (juce::FontOptions (11.0f));
    const double start = state.scrollSamples, end = state.scrollSamples + getWidth() * state.samplesPerPixel;

    // Bars: every n bars so labels are at least 50 px apart.
    const double bar = state.barSamples();
    int barStep = 1;
    while (bar * barStep / state.samplesPerPixel < 50.0)
        barStep *= 2;
    for (auto b = (juce::int64) std::floor (start / bar / barStep) * barStep; (double) b * bar < end; b += barStep)
    {
        const float x = state.timeToX ((juce::int64) ((double) b * bar));
        g.setColour (DAWColours::textDim);
        g.drawVerticalLine ((int) x, 4.0f, (float) ruler.getCentreY());
        g.setColour (DAWColours::text);
        g.drawText (juce::String (b + 1), juce::Rectangle<float> (x + 3.0f, 2.0f, 60.0f, 18.0f), juce::Justification::centredLeft);
    }

    // Minutes:seconds: a step that keeps labels at least 70 px apart.
    static const double steps[] = { 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
    double step = 600.0;
    for (const double s : steps)
        if (s * state.sampleRate / state.samplesPerPixel >= 70.0)
        {
            step = s;
            break;
        }

    const double stepSamples = step * state.sampleRate;
    for (double t = std::floor (start / stepSamples) * stepSamples; t < end; t += stepSamples)
    {
        const float x = state.timeToX ((juce::int64) t);
        g.setColour (DAWColours::textDim);
        g.drawVerticalLine ((int) x, (float) ruler.getCentreY() + 4.0f, (float) ruler.getBottom());
        const double seconds = t / state.sampleRate;
        const int minutes = (int) (seconds / 60.0);
        const double rest = seconds - minutes * 60.0;
        const auto label = juce::String (minutes) + ":" + (step < 1.0 ? juce::String (rest, step < 0.1 ? 2 : 1).paddedLeft ('0', step < 0.1 ? 5 : 4)
                                                                     : juce::String ((int) std::round (rest)).paddedLeft ('0', 2));
        g.drawText (label, juce::Rectangle<float> (x + 3.0f, (float) ruler.getCentreY() + 2.0f, 70.0f, 18.0f), juce::Justification::centredLeft);
    }
}

//==============================================================================
void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = hitClip (e.getPosition());
    auto shape = juce::MouseCursor::NormalCursor;

    if (e.y < EditState::kRulerHeight)
        shape = juce::MouseCursor::PointingHandCursor;
    else if (hit.zone == Zone::trimStart || hit.zone == Zone::trimEnd)
        shape = juce::MouseCursor::LeftRightResizeCursor;
    else if (hit.zone == Zone::fadeIn || hit.zone == Zone::fadeOut)
        shape = juce::MouseCursor::CrosshairCursor;
    else if (state.tool == EditTool::select)
        shape = juce::MouseCursor::IBeamCursor;
    else if (hit.zone == Zone::body)
        shape = juce::MouseCursor::DraggingHandCursor;

    setMouseCursor (shape);
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    drag = {};
    drag.downTime = state.xToTime ((float) e.x);

    if (e.mods.isPopupMenu())
    {
        if (const int index = trackIndexAt (e.y); index >= 0)
            actions.showTrackMenu (session.getTracks()[(size_t) index]->id);
        return;
    }

    if (e.y < EditState::kRulerHeight)
    {
        drag.kind = DragKind::ruler;
        state.cursor = state.snap (drag.downTime);
        actions.locate (state.cursor);
        state.changed();
        return;
    }

    const auto hit = hitClip (e.getPosition());
    drag.downTrackIndex = std::max (0, hit.trackIndex);

    if (hit.trackIndex >= 0)
        state.selectedTrack = session.getTracks()[(size_t) hit.trackIndex]->id;

    if (hit.clipId == 0 || (state.tool == EditTool::select && hit.zone == Zone::body))
    {
        // Empty space (or the selector on a clip): cursor and time selection.
        drag.kind = hit.trackIndex >= 0 ? DragKind::select : DragKind::none;
        state.selectedClips.clear();
        state.selection = {};
        state.cursor = state.snap (drag.downTime);
        if (! session.getEngine().isPlaying())
            actions.locate (state.cursor);
        state.changed();
        return;
    }

    drag.before = session.captureClips();
    drag.clipId = hit.clipId;
    session.setGestureActive (true);   // takes wait: the drag rebuilds from drag.before
    int trackId = 0;
    if (const auto* clip = session.findClip (hit.clipId, &trackId))
        drag.anchorStart = clip->start;

    switch (hit.zone)
    {
        case Zone::trimStart: drag.kind = DragKind::trimStart; break;
        case Zone::trimEnd:   drag.kind = DragKind::trimEnd; break;
        case Zone::fadeIn:    drag.kind = DragKind::fadeIn; break;
        case Zone::fadeOut:   drag.kind = DragKind::fadeOut; break;
        case Zone::body:
        case Zone::none:      drag.kind = DragKind::move; break;
    }

    if (drag.kind == DragKind::move)
    {
        if (e.mods.isShiftDown())
        {
            if (state.selectedClips.count (hit.clipId) != 0)
                state.selectedClips.erase (hit.clipId);
            else
                state.selectedClips.insert (hit.clipId);
        }
        else if (state.selectedClips.count (hit.clipId) == 0)
        {
            state.selectedClips = { hit.clipId };
        }

        state.selection = {};
        drag.copy = e.mods.isAltDown();

        for (const auto& [id, clips] : drag.before)
            for (const auto& c : clips)
                if (state.selectedClips.count (c.id) != 0)
                {
                    drag.moving.emplace_back (c.id, id);
                    if (drag.copy)
                        drag.copyIds[c.id] = session.newClipId();
                }

        if (state.selectedClips.count (hit.clipId) == 0)
            drag.kind = DragKind::none;   // Shift-click deselected it
    }
    else
    {
        state.selectedClips = { hit.clipId };
    }

    state.changed();
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.kind == DragKind::none || ! e.mouseWasDraggedSinceMouseDown())
        return;

    applyDrag (e);
}

void TimelineView::applyDrag (const juce::MouseEvent& e)
{
    const auto t = state.xToTime ((float) e.x);
    const auto& tracks = session.getTracks();

    if (drag.kind == DragKind::ruler)
    {
        state.cursor = state.snap (std::max<juce::int64> (0, t));
        actions.locate (state.cursor);
        state.changed();
        return;
    }

    if (drag.kind == DragKind::select)
    {
        const auto a = state.snap (std::max<juce::int64> (0, drag.downTime));
        const auto b = state.snap (std::max<juce::int64> (0, t));
        int index = trackIndexAt (juce::jlimit (EditState::kRulerHeight, getHeight() - 1, e.y));
        if (index < 0)
            index = (int) tracks.size() - 1;
        const int first = std::min (drag.downTrackIndex, index), last = std::max (drag.downTrackIndex, index);

        state.selection.start = std::min (a, b);
        state.selection.end = std::max (a, b);
        state.selection.trackIds.clear();
        for (int i = first; i <= last && i < (int) tracks.size(); ++i)
            state.selection.trackIds.insert (tracks[(size_t) i]->id);
        state.cursor = state.selection.start;
        state.changed();
        return;
    }

    auto map = drag.before;
    int trackId = 0;
    const Clip* original = nullptr;
    for (const auto& [id, clips] : drag.before)
        for (const auto& c : clips)
            if (c.id == drag.clipId)
            {
                original = &c;
                trackId = id;
            }

    if (original == nullptr)
        return;

    const auto ids = session.idSource();

    if (drag.kind == DragKind::move)
    {
        auto newStart = state.snap (drag.anchorStart + (t - drag.downTime));
        juce::int64 earliest = std::numeric_limits<juce::int64>::max();
        int lowestIndex = (int) tracks.size(), highestIndex = 0;
        for (const auto& [clipId, tid] : drag.moving)
        {
            const int idx = session.indexOfTrack (tid);
            lowestIndex = std::min (lowestIndex, idx);
            highestIndex = std::max (highestIndex, idx);
            for (const auto& c : drag.before.at (tid))
                if (c.id == clipId)
                    earliest = std::min (earliest, c.start);
        }

        auto delta = newStart - drag.anchorStart;
        delta = std::max (delta, -earliest);   // nothing before 0

        int overIndex = trackIndexAt (juce::jlimit (EditState::kRulerHeight, getHeight() - 1, e.y));
        if (overIndex < 0)
            overIndex = (int) tracks.size() - 1;
        int trackDelta = overIndex - drag.downTrackIndex;
        trackDelta = juce::jlimit (-lowestIndex, (int) tracks.size() - 1 - highestIndex, trackDelta);

        std::vector<std::pair<Clip, int>> placed;
        for (const auto& [clipId, tid] : drag.moving)
        {
            auto& list = map[tid];
            const int index = ClipEdit::indexOf (list, clipId);
            if (index < 0)
                continue;

            auto clip = list[(size_t) index];
            if (! drag.copy)
                list.erase (list.begin() + index);
            else
                clip.id = drag.copyIds[clipId];

            clip.start += delta;
            const int newIndex = session.indexOfTrack (tid) + trackDelta;
            placed.emplace_back (clip, tracks[(size_t) newIndex]->id);
        }

        for (auto& [clip, tid] : placed)
            ClipEdit::place (map[tid], clip, ids);

        if (drag.copy)
        {
            state.selectedClips.clear();
            for (const auto& [from, to] : drag.copyIds)
                state.selectedClips.insert (to);
        }
    }
    else
    {
        auto& list = map[trackId];
        const int index = ClipEdit::indexOf (list, drag.clipId);
        auto clip = list[(size_t) index];
        // Edges and fades move by the mouse's movement, so grabbing an edge
        // a few pixels off does not make it jump.
        const auto delta = t - drag.downTime;

        switch (drag.kind)
        {
            case DragKind::trimStart: ClipEdit::trimStart (clip, std::max<juce::int64> (0, state.snap (original->start + delta))); break;
            case DragKind::trimEnd:   ClipEdit::trimEnd (clip, state.snap (original->end() + delta)); break;
            case DragKind::fadeIn:    clip.fadeIn = juce::jlimit<juce::int64> (0, clip.length - clip.fadeOut, original->fadeIn + delta); break;
            case DragKind::fadeOut:   clip.fadeOut = juce::jlimit<juce::int64> (0, clip.length - clip.fadeIn, original->fadeOut - delta); break;
            case DragKind::move: case DragKind::select: case DragKind::ruler: case DragKind::none: break;
        }

        list.erase (list.begin() + index);
        ClipEdit::place (list, clip, ids);
    }

    session.applyClips (map);
    drag.changed = true;
}

void TimelineView::mouseUp (const juce::MouseEvent&)
{
    if (drag.changed)
    {
        const char* names[] = { "", "클립 이동", "트림", "트림", "페이드 인", "페이드 아웃", "", "" };
        auto name = utf8 (names[(int) drag.kind]);
        if (drag.kind == DragKind::move && drag.copy)
            name = utf8 ("클립 복사");
        session.commitClipEdit (name, drag.before);
    }

    drag = {};
    session.setGestureActive (false);
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = hitClip (e.getPosition());
    if (hit.clipId == 0 || hit.trackIndex < 0)
        return;

    if (const auto* clip = session.findClip (hit.clipId))
    {
        // Selects the clip's range on its track (any tool).
        state.selection.start = clip->start;
        state.selection.end = clip->end();
        state.selection.trackIds = { session.getTracks()[(size_t) hit.trackIndex]->id };
        state.selectedClips = { clip->id };
        state.cursor = clip->start;
        state.changed();
    }
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        state.zoom (wheel.deltaY > 0 ? 0.8 : 1.25, (float) e.x);
    }
    else if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        state.setScroll (state.scrollSamples - (double) delta * 300.0 * state.samplesPerPixel);
    }
    else
    {
        const int visible = getHeight() - EditState::kRulerHeight;
        const int maxScroll = std::max (0, getLanesHeight() - visible + state.trackHeight / 2);
        state.scrollY = juce::jlimit (0, maxScroll, state.scrollY - (int) (wheel.deltaY * 200.0f));
        state.changed();
    }

    if (onScrollChanged)
        onScrollChanged();
}

bool TimelineView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void TimelineView::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::StringArray audio;
    for (const auto& f : files)
        if (isAudioFile (f))
            audio.add (f);

    const int index = trackIndexAt (y);
    const int trackId = index >= 0 ? session.getTracks()[(size_t) index]->id : 0;
    actions.importFiles (audio, trackId, state.snap (std::max<juce::int64> (0, state.xToTime ((float) x))));
}
