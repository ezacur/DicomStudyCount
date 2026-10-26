// The DICOM explorer: the floating thumbnail of the node under the mouse
#include "Shell.h"

// ----- the floating thumbnail of the node under the mouse

LRESULT CALLBACK DicomExplorer::TreeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    auto self = reinterpret_cast<DicomExplorer*>(ref);
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSEHOVER)
    {
        if (self->hoverTree != h) { self->HidePopup(); self->hoverItem = nullptr; self->hoverArmed = false; }
        self->hoverTree = h;
    }
    if (msg == WM_MOUSEMOVE) self->Mark(h, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
    if (msg == WM_MOUSELEAVE) { self->markTracking = false; self->Mark(nullptr, {}); }
    switch (msg)
    {
    case WM_MOUSEMOVE: self->HoverMove({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); break;
    case WM_MOUSEHOVER:
        // (it is ours: the tree, without its tooltips, falls over its own handling of it)
        self->hoverArmed = false;
        self->HoverShow({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;
    case WM_MOUSELEAVE: self->hoverArmed = false; self->hoverItem = nullptr; self->HidePopup(); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        // A check box: ours (what the navigator draws, for everything under the node), not the tree's
        self->HidePopup();
        TVHITTESTINFO hit = {};
        hit.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HTREEITEM item = TreeView_HitTest(h, &hit);
        if (item && h == self->tree && (hit.flags & TVHT_ONITEMSTATEICON))
        {
            TVITEMW tv = { TVIF_PARAM };
            tv.hItem = item;
            if (msg == WM_LBUTTONDOWN && TreeView_GetItem(h, &tv)) self->Toggle(static_cast<int>(tv.lParam));
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        self->HidePopup();
        if (wp == VK_SPACE && h == self->tree) { self->Toggle(self->selected); return 0; }
        break;
    case WM_CHAR:
        if (wp == L' ' && h == self->tree) return 0;
        break;
    case WM_MBUTTONDOWN:
    {
        // The middle button on a node: to the other tree (used / unused)
        self->HidePopup();
        TVHITTESTINFO hit = {};
        hit.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HTREEITEM item = TreeView_HitTest(h, &hit);
        TVITEMW tv = { TVIF_PARAM };
        tv.hItem = item;
        if (item && (hit.flags & TVHT_ONITEM) && TreeView_GetItem(h, &tv)) self->MoveUnused(static_cast<int>(tv.lParam), h == self->tree);
        return 0;
    }
    case WM_RBUTTONDOWN: case WM_MOUSEWHEEL: case WM_VSCROLL: case WM_HSCROLL: case WM_KILLFOCUS:
        self->HidePopup();
        break;
    case WM_NCDESTROY: RemoveWindowSubclass(h, TreeProc, 1); break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

int DicomExplorer::NodeAt(POINT pt, HTREEITEM* where) const
{
    TVHITTESTINFO hit = {};
    hit.pt = pt;
    HWND t = hoverTree ? hoverTree : tree;
    HTREEITEM item = TreeView_HitTest(t, &hit);
    if (!item || !(hit.flags & TVHT_ONITEM)) return -1;
    if (where) *where = item;
    TVITEMW tv = { TVIF_PARAM };
    tv.hItem = item;
    return TreeView_GetItem(t, &tv) ? static_cast<int>(tv.lParam) : -1;
}

// The mouse on another node: the thumbnail goes, and comes back if it stays still there for a moment
void DicomExplorer::HoverMove(POINT pt)
{
    if (!ready || !hoverPreview) return;
    HTREEITEM item = nullptr;
    NodeAt(pt, &item);
    if (item != hoverItem)
    {
        hoverItem = item;
        HidePopup();
        hoverArmed = false;
    }
    if (!hoverArmed && item && (!popup || !IsWindowVisible(popup)))
    {
        TRACKMOUSEEVENT t = { sizeof(t), TME_HOVER | TME_LEAVE, hoverTree ? hoverTree : tree, 450 };
        hoverArmed = TrackMouseEvent(&t) != FALSE;
    }
}

void DicomExplorer::HoverShow(POINT pt)
{
    if (!ready || !hoverPreview || !popup) return;
    HTREEITEM item = nullptr;
    const int n = NodeAt(pt, &item);
    if (n <= 0 || n >= static_cast<int>(nodes.size())) return;
    popupNode = n;
    popupFrame = 0;
    hoverItem = item;
    if (!thumbs.count(n)) AskThumb(n);
    PlacePopup();
}

void DicomExplorer::HidePopup()
{
    if (!popup) return;
    KillTimer(popup, TimerFrame);
    if (IsWindowVisible(popup)) ShowWindow(popup, SW_HIDE);
    popupNode = -1;
}

// What the thumbnail of a node shows (DCMthumbnail.m): an image, its frames; a position, its images
// (the cine); an orientation, its positions as a mosaic (each one moving, a cine of several slices); a
// series of several orientations, one over the other; a study or a patient, a mosaic of its series
Stack DicomExplorer::StackOf(int n) const
{
    const GatherNode& node = nodes[n];
    const std::vector<Gathered>& files = data->files;
    Stack s;
    if (node.level == Level::Image)
    {
        const Gathered& g = files[node.files[0]];
        s.files = { { { g.path, (std::max)(0, g.frame) } } };
        s.frames = g.frame >= 0 ? 1 : g.frames;     // (a frame of an enhanced file: that one)
        return s;
    }
    std::vector<int> positions;
    if (node.level == Level::Position) positions = { n };
    else
    {
        int o = n;
        while (nodes[o].level != Level::Orientation && !nodes[o].children.empty()) o = nodes[o].children[0];
        positions = nodes[o].children;
    }
    for (int p : positions)
    {
        std::vector<FrameRef> paths;
        for (int f : nodes[p].files) paths.push_back({ files[f].path, (std::max)(0, files[f].frame) });
        s.files.push_back(std::move(paths));
    }
    if (s.files.size() == 1 && s.files[0].size() == 1 && files[nodes[positions[0]].files[0]].frame < 0) s.frames = files[nodes[positions[0]].files[0]].frames;
    // The time from image to image: the median step of the trigger times of the first position
    std::vector<double> t;
    for (int f : nodes[positions[0]].files) if (!files[f].trigger.empty()) t.push_back(atof(files[f].trigger.c_str()));
    if (t.size() > 1)
    {
        std::vector<double> steps;
        for (size_t k = 1; k < t.size(); k++) steps.push_back(std::fabs(t[k] - t[k - 1]));
        std::sort(steps.begin(), steps.end());
        s.delay = steps[steps.size() / 2];
    }
    return s;
}

// The orientation with most positions of a series (the one that shows it best)
int DicomExplorer::MainOrientation(int series) const
{
    int best = nodes[series].children[0];
    for (int o : nodes[series].children) if (nodes[o].children.size() > nodes[best].children.size()) best = o;
    return best;
}

void DicomExplorer::AskThumb(int n, bool big)
{
    ThumbAsk a;
    a.node = n;
    a.generation = generation;
    a.big = big;
    const COLORREF bg = GetSysColor(big ? COLOR_WINDOW : COLOR_INFOBK);     // (behind a cascade: the panel's, or the popup's)
    a.background = (static_cast<DWORD>(GetRValue(bg)) << 16) | (GetGValue(bg) << 8) | GetBValue(bg);
    const GatherNode& node = nodes[n];
    if (node.level == Level::Series && node.children.size() > 1)
    {
        a.kind = ThumbAsk::Cascade;
        for (int o : node.children) a.stacks.push_back(StackOf(o));
    }
    else if (node.level == Level::Study || node.level == Level::Patient)
    {
        a.kind = ThumbAsk::Grid;
        std::function<void(int)> walk = [&](int k) {
            if (a.stacks.size() >= 20) return;
            if (nodes[k].level == Level::Series) { a.stacks.push_back(StackOf(MainOrientation(k))); return; }
            for (int c : nodes[k].children) walk(c);
        };
        walk(n);
    }
    else a.stacks.push_back(StackOf(n));
    if (worker) worker->Ask(std::move(a));
}

void DicomExplorer::Thumbnailed(ThumbDone* d)
{
    std::unique_ptr<ThumbDone> done(d);
    if (d->generation != generation || d->node <= 0 || d->node >= static_cast<int>(nodes.size())) return;
    if (d->big)
    {
        posters[d->node] = std::make_shared<Thumb>(std::move(d->thumb));
        ShowPoster(d->node);
        return;
    }
    thumbs[d->node] = std::make_shared<Thumb>(std::move(d->thumb));
    if (popupNode == d->node) { popupFrame = 0; PlacePopup(); }
}

// The line of the node (and what the thumbnail shows of it) under the image
std::wstring DicomExplorer::PopupText() const
{
    if (popupNode <= 0) return std::wstring();
    std::wstring s = nodes[popupNode].text;
    auto it = thumbs.find(popupNode);
    if (it == thumbs.end()) s += L"\nReading...";
    else if (!it->second->error.empty()) s += L"\nNo image: " + it->second->error;
    else
    {
        if (!it->second->caption.empty()) s += L"\n" + it->second->caption;
        const GatherNode& node = nodes[popupNode];
        if (node.level == Level::Series && node.children.size() > 1) s += L"\n" + std::to_wstring(node.children.size()) + L" orientations, their middle images";
        if (node.level == Level::Study || node.level == Level::Patient)
        {
            size_t k = 0;
            std::function<void(int)> walk = [&](int c) { if (nodes[c].level == Level::Series) { k++; return; } for (int d : nodes[c].children) walk(d); };
            walk(popupNode);
            s += L"\n" + std::wstring(k > 20 ? L"20 of " : L"") + std::to_wstring(k) + L" series, their middle images";
        }
    }
    return s;
}

int DicomExplorer::PopupScale() const { return (std::max)(1, static_cast<int>((GetDpiForWindow(wnd) + 48) / 96)); }

// The popup beside the node, its size that of the image and the text, inside the screen
void DicomExplorer::PlacePopup()
{
    if (!popup || popupNode <= 0 || !hoverItem) return;
    auto it = thumbs.find(popupNode);
    const Thumb* t = it != thumbs.end() && !it->second->frames.empty() ? it->second.get() : nullptr;
    const int k = PopupScale(), m = Px(6);
    const int iw = t ? t->frames[0].width * k : 0, ih = t ? t->frames[0].height * k : 0;
    HDC dc = GetDC(popup);
    HGDIOBJ old = SelectObject(dc, font);
    RECT text = { 0, 0, (std::max)(iw, Px(260)), 0 };
    const std::wstring s = PopupText();
    DrawTextW(dc, s.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(popup, dc);
    const int w = (std::max)(iw, static_cast<int>(text.right)) + 2 * m + 2, h = ih + (ih ? m : 0) + text.bottom + 2 * m + 2;
    RECT r = {};
    HWND under = hoverTree ? hoverTree : tree;
    TreeView_GetItemRect(under, hoverItem, &r, TRUE);
    POINT at = { r.right + Px(16), r.top };
    ClientToScreen(under, &at);
    POINT cursor;
    GetCursorPos(&cursor);
    at.x = (std::max)(at.x, cursor.x + Px(24));
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    if (at.x + w > work.right) at.x = (std::max)(static_cast<LONG>(work.left), cursor.x - Px(24) - w);
    if (at.y + h > work.bottom) at.y = (std::max)(work.top, work.bottom - h);
    SetWindowPos(popup, HWND_TOPMOST, at.x, at.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(popup, nullptr, TRUE);
    KillTimer(popup, TimerFrame);
    if (t && t->frames.size() > 1) SetTimer(popup, TimerFrame, static_cast<UINT>(t->delay), nullptr);
}

void DicomExplorer::PaintPopup(HDC dc)
{
    RECT c;
    GetClientRect(popup, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_INFOBK));
    if (popupNode <= 0) return;
    const int k = PopupScale(), m = Px(6);
    int y = m;
    auto it = thumbs.find(popupNode);
    if (it != thumbs.end() && !it->second->frames.empty())
    {
        const Picture& p = it->second->frames[popupFrame % it->second->frames.size()];
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = p.width;
        bi.bmiHeader.biHeight = -p.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, m, y, p.width * k, p.height * k, 0, 0, p.width, p.height, p.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        y += p.height * k + m;
    }
    RECT text = { m, y, c.right - m, c.bottom - m };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_INFOTEXT));
    HGDIOBJ old = SelectObject(dc, font);
    const std::wstring s = PopupText();
    DrawTextW(dc, s.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
}

LRESULT CALLBACK DicomExplorer::PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<DicomExplorer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self)
        {
            // (drawn aside first: no flicker while it moves)
            RECT c;
            GetClientRect(hwnd, &c);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            self->PaintPopup(mem);
            BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (self && wp == TimerFrame)
        {
            self->popupFrame++;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
