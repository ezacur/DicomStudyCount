// The preview: going through the frames and the files - the slider, the times and the places, the cine,
// the jumps, the keys
#include "Shell.h"

// The slider of the image bar: the frames of a multi-frame file, or else the files of its series
// (the frames of enhanced files, as images of their own: the slider over them, as over files)
int PreviewHandler::Count() const { return view.frames > 1 && seriesFrames.empty() ? view.frames : static_cast<int>(series.size()); }

int PreviewHandler::Position() const { return view.frames > 1 && seriesFrames.empty() ? view.frame : slice; }

void PreviewHandler::GoTo(int i)
{
    if (view.frames > 1 && seriesFrames.empty()) GoFrame(i);
    else GoSlice(i);
}

void PreviewHandler::Step(int delta)
{
    const int n = Count();
    if (!poster.empty() && n > 0) GoTo(0);
    else if (n > 1 && Position() >= 0) GoTo((std::max)(0, (std::min)(n - 1, Position() + delta)));
}

// Another frame of the file, with the same window
void PreviewHandler::GoFrame(int f)
{
    if (f < 0 || f >= view.frames || f == view.frame || !view.imaging) return;
    const bool had = !view.pixels.empty();
    std::wstring why;
    try { why = ShowFrame(view, f, true); }
    catch (...) { why = L"it cannot be shown"; }
    if (!why.empty()) view.frame = f;           // no image, and the stepping goes on from there
    Shown(had);
    if (hosted && moved && (playing || jumping)) moved(slice);     // (the explorer draws it where it is)
}

// Another file of the series: the window dragged stays, and the zoom while the size of the image
// does. Its header and the values of its columns, when the stepping stops (Settle).
void PreviewHandler::GoSlice(int i)
{
    if (i < 0 || i >= static_cast<int>(series.size()) || i == slice) return;
    StopPoster();
    // Another frame of the enhanced file shown: only that frame, the file not read again
    const int wanted = i < static_cast<int>(seriesFrames.size()) ? seriesFrames[i] : -1;
    if (wanted >= 0 && slice >= 0 && series[i] == current && view.imaging && poster.empty())
    {
        const bool had = !view.pixels.empty();
        std::wstring why;
        try { why = ShowFrame(view, wanted, true); }
        catch (...) { why = L"it cannot be shown"; }
        view.frame = wanted;
        slice = i;
        if (hosted && moved) moved(i);
        Shown(had);
        return;
    }
    Rendered next;
    try
    {
        std::vector<char> data;
        if (ReadPath(series[i], data)) next = RenderDicom(data, Render::Image);
        else next.summary = L"This file cannot be read";
        if (wanted > 0 && next.imaging) ShowFrame(next, wanted, false);
    }
    catch (...) { next = Rendered(); next.summary = L"This file cannot be shown"; }
    Imaging* before = view.imaging.get();
    Imaging* after = next.imaging.get();
    if (windowSet && before && after && after->image && before->mono == after->mono)
    {
        after->center = before->center;
        after->width = before->width;
        Rewindow(next);
    }
    const bool had = !view.pixels.empty();
    if (next.width != view.width || next.height != view.height) zoom = 1;
    view = std::move(next);
    slice = i;
    current = series[i];
    stale = !hosted;                            // (hosted: no header here to write)
    if (stale) SetTimer(wnd, TimerSettle, 300, nullptr);
    SetWindowTextW(wnd, view.summary.c_str());
    if (hosted && moved) moved(i);
    // In a DICOMDIR: the file in the tree too, when its series is open there
    if (tree && group >= 0 && group < static_cast<int>(groups.size()) && i < static_cast<int>(groups[group].items.size()) &&
        !selecting)
    {
        selecting = true;
        TreeView_SelectItem(tree, groups[group].items[i]);
        selecting = false;
    }
    Shown(had);
}

// After a frame or a slice: the image, its bar and the status line again (all, if the image came or went)
void PreviewHandler::Shown(bool had)
{
    KeepView();
    if (had != !view.pixels.empty())
    {
        Layout();
        return;
    }
    InvalidateRect(wnd, &imageArea, FALSE);
    InvalidateRect(wnd, &barArea, FALSE);
    InvalidateRect(wnd, &statusArea, FALSE);
}

// When the stepping stops: the header and the values of the columns of the file shown
void PreviewHandler::Settle()
{
    if (wnd) KillTimer(wnd, TimerSettle);
    if (!stale) return;
    stale = false;
    try
    {
        if (view.imaging && view.imaging->file) DumpHeader(*view.imaging->file, view.tags, view.names);
    }
    catch (...) { view.tags = view.names = L"(the header cannot be shown)"; }
    if (edit)
    {
        const LRESULT first = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        SetWindowTextW(edit, (tab == TabTags ? view.tags : view.names).c_str());
        SendMessageW(edit, EM_LINESCROLL, 0, first);
    }
    if (list) FillValues(current);
}

// The series has been read: where this file is in it
void PreviewHandler::Series()
{
    std::vector<std::wstring> files = std::move(scan->files);
    scan.reset();
    for (size_t i = 0; i < files.size(); i++)
        if (CompareStringOrdinal(files[i].c_str(), -1, current.c_str(), -1, TRUE) == CSTR_EQUAL)
        {
            slice = static_cast<int>(i);
            series = std::move(files);
            break;
        }
    InvalidateRect(wnd, &barArea, FALSE);
}

// The places of the files: in space (planes or series) and in time (the files of each position)
void PreviewHandler::Places()
{
    spaces.clear();
    times.clear();
    spaceIndex.assign(series.size(), 0);
    std::map<int, size_t> at;
    for (size_t i = 0; i < series.size(); i++)
    {
        const int key = spaceOf.empty() ? static_cast<int>(i) : spaceOf[i];
        auto r = at.emplace(key, spaces.size());
        if (r.second) spaces.emplace_back();
        spaces[r.first->second].push_back(static_cast<int>(i));
        spaceIndex[i] = static_cast<int>(r.first->second);
        times[timeOf.empty() ? static_cast<int>(i) : timeOf[i]].push_back(static_cast<int>(i));
    }
}

// How many times the image shown has (the frames of its file, or the images of its position), and how
// many places its files have
bool PreviewHandler::OverFrames() const { return view.frames > 1 && seriesFrames.empty(); }

int PreviewHandler::TimeCount() const
{
    if (OverFrames()) return view.frames;
    if (slice < 0 || slice >= static_cast<int>(series.size()) || timeOf.empty()) return poster.empty() ? 1 : FirstTimes();
    auto it = times.find(timeOf[slice]);
    return it == times.end() ? 1 : static_cast<int>(it->second.size());
}

int PreviewHandler::FirstTimes() const      // (the thumbnail shown: the times of the first file)
{
    if (timeOf.empty() || series.empty()) return 1;
    auto it = times.find(timeOf[0]);
    return it == times.end() ? 1 : static_cast<int>(it->second.size());
}

int PreviewHandler::SpaceCount() const
{
    if (series.size() != spaceIndex.size()) return static_cast<int>(series.size());
    return static_cast<int>(spaces.size());
}

// Where the image shown is in time (its frame, or among the images of its position) and in space
int PreviewHandler::TimeIndex() const
{
    if (OverFrames()) return view.frame;
    if (slice < 0 || timeOf.empty()) return 0;
    auto it = times.find(timeOf[slice]);
    if (it == times.end()) return 0;
    return static_cast<int>(std::find(it->second.begin(), it->second.end(), slice) - it->second.begin());
}

int PreviewHandler::SpaceIndex() const
{
    if (slice < 0) return 0;
    if (series.size() != spaceIndex.size()) return slice;
    return spaceIndex[slice];
}

// To a time of the image shown (a frame, or an image of its position)
void PreviewHandler::GoTime(int k)
{
    if (OverFrames()) { GoFrame(k); return; }
    if (series.empty() || timeOf.empty()) return;
    const int base = slice >= 0 ? slice : 0;
    const std::vector<int>& same = times[timeOf[base]];
    if (k >= 0 && k < static_cast<int>(same.size())) GoSlice(same[k]);
}

// To a place: the plane (at the same time, if it has it) or the series
void PreviewHandler::GoSpace(int target)
{
    const int n = SpaceCount();
    if (n < 1 || target < 0 || target >= n) return;
    if (series.size() != spaceIndex.size()) { GoSlice(target); return; }
    int phase = 0;
    if (slice >= 0 && !timeOf.empty())
    {
        const std::vector<int>& mine = times[timeOf[slice]];
        phase = static_cast<int>(std::find(mine.begin(), mine.end(), slice) - mine.begin());
    }
    const std::vector<int>& next = spaces[target];
    std::vector<int> first;                     // (its first position)
    for (int f : next) if (timeOf.empty() || timeOf[f] == timeOf[next[0]]) first.push_back(f);
    GoSlice(first[(std::min)(phase, static_cast<int>(first.size()) - 1)]);
}

// The next time: the next frame, or the next image of the position, round and round
void PreviewHandler::StepTime(int delta)
{
    if (OverFrames()) { GoFrame(((view.frame + delta) % view.frames + view.frames) % view.frames); return; }
    if (slice < 0 || timeOf.empty()) return;
    const std::vector<int>& same = times[timeOf[slice]];
    const int k = static_cast<int>(std::find(same.begin(), same.end(), slice) - same.begin());
    const int n = static_cast<int>(same.size());
    if (n > 1) GoSlice(same[((k + delta) % n + n) % n]);
}

// The next place: the next plane (at the same time, if it has it) or series, round and round
void PreviewHandler::StepSpace(int delta)
{
    const int n = SpaceCount();
    if (n < 2) return;
    if (!poster.empty() && slice < 0) { GoSlice(0); return; }
    GoSpace(((SpaceIndex() + delta) % n + n) % n);
}

// The cine: the frames at the pace of the file, the images of a position at 10 a second, round and round
void PreviewHandler::Play(bool on)
{
    if (on && !poster.empty()) GoSlice(0);     // (from the thumbnail: the files, from the first one)
    if (on) Jump(false);
    playing = on && TimeCount() > 1;
    if (playing)
    {
        const double ms = OverFrames() && view.frameTime > 0 ? view.frameTime : 100;
        SetTimer(wnd, TimerCine, static_cast<UINT>((std::max)(15.0, (std::min)(1000.0, ms))), nullptr);
    }
    else
    {
        KillTimer(wnd, TimerCine);
        if (stale && !jumping) SetTimer(wnd, TimerSettle, 300, nullptr);     // the header of the slice it stopped at
    }
    InvalidateRect(wnd, &barArea, FALSE);
    if (running) running(playing || jumping);
}

// The jumps: plane after plane (or series after series), round and round
void PreviewHandler::Jump(bool on)
{
    if (on && !poster.empty()) GoSlice(0);
    if (on) Play(false);
    jumping = on && SpaceCount() > 1;
    if (jumping) SetTimer(wnd, TimerJump, static_cast<UINT>((std::max)(15, (std::min)(2000, jumpDelay))), nullptr);
    else
    {
        if (wnd) KillTimer(wnd, TimerJump);
        if (stale && !playing && wnd) SetTimer(wnd, TimerSettle, 300, nullptr);
    }
    if (wnd) InvalidateRect(wnd, &barArea, FALSE);
    if (running) running(playing || jumping);
}

// The keys of the image, when it has the focus (a click on it): Left / Right the frames (or the
// slices), Up / Down and Page Up / Page Down the slices, Home / End the first and the last, Space
// the cine
bool PreviewHandler::Key(WPARAM key)
{
    if (view.pixels.empty() && Count() < 2) return false;
    switch (key)
    {
    case VK_LEFT: if (TimeCount() > 1) StepTime(-1); else Step(-1); return true;
    case VK_RIGHT: if (TimeCount() > 1) StepTime(1); else Step(1); return true;
    case VK_UP: case VK_PRIOR: if (!poster.empty()) GoSlice(0); else StepSpace(-1); return true;
    case VK_DOWN: case VK_NEXT: if (!poster.empty()) GoSlice(0); else StepSpace(1); return true;
    case VK_HOME: GoTo(0); return true;
    case VK_END: GoTo(Count() - 1); return true;
    case VK_SPACE: if (TimeCount() > 1) Play(!playing); else Jump(!jumping); return true;
    }
    return false;
}

// Dragging along the slider of the places
void PreviewHandler::TrackSpaceTo(POINT pt)
{
    const int n = SpaceCount();
    if (n < 2 || spaceTrackRect.right <= spaceTrackRect.left) return;
    const double t = static_cast<double>(pt.x - spaceTrackRect.left) / (spaceTrackRect.right - spaceTrackRect.left);
    const int k = static_cast<int>(std::lround((std::max)(0.0, (std::min)(1.0, t)) * (n - 1)));
    if (k != SpaceIndex() || slice < 0) GoSpace(k);
}

// Dragging along the slider of the times
void PreviewHandler::TrackTo(POINT pt)
{
    const int nt = TimeCount();
    if (nt > 1 && trackRect.right > trackRect.left)
    {
        const double t = static_cast<double>(pt.x - trackRect.left) / (trackRect.right - trackRect.left);
        const int k = static_cast<int>(std::lround((std::max)(0.0, (std::min)(1.0, t)) * (nt - 1)));
        if (!poster.empty() && slice < 0) GoSlice(0);
        if (k != TimeIndex()) GoTime(k);
    }
}
