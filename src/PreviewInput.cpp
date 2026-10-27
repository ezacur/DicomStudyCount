// The preview: the window, the zoom and the pan dragged over the image, and its window procedure
#include "Shell.h"

// Dragging over the image: right widens the window (less contrast), up lowers its center (brighter).
// The whole range of the pixels in the height of the image area.
void PreviewHandler::MoveWindowTo(POINT pt)
{
    Imaging* im = view.imaging.get();
    if (!im) return;
    const int area = imageArea.bottom - imageArea.top > Px(100) ? imageArea.bottom - imageArea.top : Px(100);
    const double step = (im->high - im->low > 1 ? im->high - im->low : 1) / area;
    im->width = dragWidth + (pt.x - dragFrom.x) * step;
    im->center = dragCenter + (pt.y - dragFrom.y) * step;
    if (im->width < 1) im->width = 1;
    windowSet = true;
    Redraw();
}

// Screen pixels per image pixel: the fit into the image area, times the zoom
double PreviewHandler::Scale() const
{
    if (view.pixels.empty() || view.width <= 0 || view.height <= 0) return 0;
    const double fit = (std::min)(static_cast<double>(imageArea.right - imageArea.left) / view.width,
                                  static_cast<double>(imageArea.bottom - imageArea.top) / (view.height * Aspect()));
    return fit > 0 ? fit * zoom : 0;
}

// The height of a pixel on the screen for its width: as its spacing says (Scale() along the rows, this
// times it down the columns)
double PreviewHandler::Aspect() const { return view.aspect > 0 ? view.aspect : 1; }

// The point of the image at the center of the area: inside the image, the middle of it unzoomed
void PreviewHandler::KeepView()
{
    if (zoom <= 1)
    {
        zoom = 1;
        viewX = view.width / 2.0;
        viewY = view.height / 2.0;
    }
    viewX = (std::max)(0.0, (std::min)(static_cast<double>(view.width), viewX));
    viewY = (std::max)(0.0, (std::min)(static_cast<double>(view.height), viewY));
}

// Dragging with the right button: up zooms in, down out (x2 every 100 pixels), around where it started
void PreviewHandler::ZoomTo(POINT pt)
{
    SetZoom(dragFrom, dragZoom * std::pow(2.0, (dragFrom.y - pt.y) / static_cast<double>(Px(100))));
}

// A new zoom (1 to 64), the image point at `pt` staying there
void PreviewHandler::SetZoom(POINT pt, double next)
{
    const double before = Scale();
    if (before <= 0) return;
    const double cx = (imageArea.left + imageArea.right) / 2.0, cy = (imageArea.top + imageArea.bottom) / 2.0;
    const double px = viewX + (pt.x - cx) / before, py = viewY + (pt.y - cy) / (before * Aspect());
    zoom = (std::max)(1.0, (std::min)(64.0, next));
    const double after = Scale();
    viewX = px - (pt.x - cx) / after;
    viewY = py - (pt.y - cy) / (after * Aspect());
    KeepView();
    InvalidateRect(wnd, &imageArea, FALSE);
    InvalidateRect(wnd, &statusArea, FALSE);
}

// On the image: the left button drags the window, the right one the zoom, the middle one (or Shift
// and the left one) the image itself
void PreviewHandler::StartImageDrag(Drag what, POINT pt)
{
    drag = what;
    dragFrom = pt;
    if (view.imaging)
    {
        dragCenter = view.imaging->center;
        dragWidth = view.imaging->width;
    }
    dragViewX = viewX;
    dragViewY = viewY;
    dragZoom = zoom;
    SetCapture(wnd);
}

// Dragging with the middle button (or Shift and the left one): the zoomed image follows the mouse
void PreviewHandler::PanTo(POINT pt)
{
    const double scale = Scale();
    if (scale <= 0) return;
    viewX = dragViewX - (pt.x - dragFrom.x) / scale;
    viewY = dragViewY - (pt.y - dragFrom.y) / (scale * Aspect());
    KeepView();
    InvalidateRect(wnd, &imageArea, FALSE);
}

void PreviewHandler::Redraw()
{
    Rewindow(view);
    InvalidateRect(wnd, &imageArea, FALSE);
    InvalidateRect(wnd, &statusArea, FALSE);
}

LRESULT CALLBACK PreviewHandler::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<PreviewHandler*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_SIZE:
        if (self && self->wnd) self->Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_READCOLUMNS:
        if (self && self->list) self->ReadColumns();
        return 0;
    case WM_SERIES:
        if (self && self->scan) self->Series();
        return 0;
    case WM_SEGMENTS:
        if (self && self->scan)
        {
            self->nearby = std::move(self->scan->segmentations);
            self->scan.reset();
            self->PaneSegments();
            if (self->list) self->FillValues(self->current);
            self->Layout();
        }
        return 0;
    case WM_TIMER:
        if (self && wp == TimerSettle)
        {
            if (self->playing || self->jumping) KillTimer(hwnd, TimerSettle);     // again when it stops
            else self->Settle();
        }
        else if (self && wp == TimerCine)
        {
            if (self->TimeCount() < 2 || self->Position() < 0) self->Play(false);
            else self->StepTime(1);
        }
        else if (self && wp == TimerJump)
        {
            if (self->SpaceCount() < 2) self->Jump(false);
            else self->StepSpace(1);
        }
        else if (self && wp == TimerPoster && !self->poster.empty())
        {
            self->posterFrame++;
            self->PosterPixels();
            InvalidateRect(hwnd, &self->imageArea, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (self && self->Key(wp)) return 0;
        break;
    case WM_SETCURSOR:
        if (self && LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            for (const RECT& r : self->tabRects) if (PtInRect(&r, pt)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
            for (const RECT* r : { &self->playRect, &self->jumpRect, &self->trackRect, &self->spaceTrackRect, &self->overlayRect, &self->segmentsRect })
                if (PtInRect(r, pt)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
            if (PtInRect(&self->splitArea, pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE; }
            if (self->view.imaging && PtInRect(&self->imageArea, pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN:
        if (self)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            for (int t = 0; t < TabCount; t++) if (PtInRect(&self->tabRects[t], pt)) { self->SelectTab(t); return 0; }
            // The image and its bar take the keys (frames, slices) after a click
            if (PtInRect(&self->imageArea, pt) || PtInRect(&self->barArea, pt)) ::SetFocus(hwnd);
            if (PtInRect(&self->splitArea, pt)) { self->drag = Drag::Split; self->dragFrom = pt; SetCapture(hwnd); }
            else if (PtInRect(&self->playRect, pt)) self->Play(!self->playing);
            else if (PtInRect(&self->jumpRect, pt)) self->Jump(!self->jumping);
            else if (PtInRect(&self->overlayRect, pt))
            {
                self->SetOverlay(!self->overlay);
            }
            else if (PtInRect(&self->segmentsRect, pt)) self->SetSegmentsShown(!self->segments);
            else if (PtInRect(&self->trackRect, pt))
            {
                self->drag = Drag::Track;
                SetCapture(hwnd);
                self->TrackTo(pt);
            }
            else if (PtInRect(&self->spaceTrackRect, pt))
            {
                self->drag = Drag::TrackSpace;
                SetCapture(hwnd);
                self->TrackSpaceTo(pt);
            }
            else if (self->view.imaging && PtInRect(&self->imageArea, pt))
                self->StartImageDrag((wp & MK_SHIFT) ? Drag::Pan : Drag::Window, pt);
        }
        return 0;
    case WM_MOUSEMOVE:
        // Hosted: where the mouse is on the image (the explorer shows it in its navigator)
        if (self && self->hosted && self->pointed && self->drag == Drag::None)
        {
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const double scale = self->Scale();
            bool inside = false;
            double column = 0, row = 0;
            if (scale > 0 && self->poster.empty() && self->slice >= 0 && PtInRect(&self->imageArea, pt))
            {
                const double left = (self->imageArea.left + self->imageArea.right) / 2.0 - self->viewX * scale;
                const double down = scale * self->Aspect();
                const double top = (self->imageArea.top + self->imageArea.bottom) / 2.0 - self->viewY * down;
                column = (pt.x - left) / scale - 0.5;
                row = (pt.y - top) / down - 0.5;
                inside = column >= -0.5 && row >= -0.5 && column < self->view.width - 0.5 && row < self->view.height - 0.5;
            }
            if (inside && !self->pointing)
            {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&t);
            }
            if (inside || self->pointing) self->pointed(inside, column, row);
            self->pointing = inside;
        }
        if (self && self->drag != Drag::None)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (self->drag == Drag::Window) self->MoveWindowTo(pt);
            else if (self->drag == Drag::Pan) self->PanTo(pt);
            else if (self->drag == Drag::Zoom) self->ZoomTo(pt);
            else if (self->drag == Drag::Track) self->TrackTo(pt);
            else if (self->drag == Drag::TrackSpace) self->TrackSpaceTo(pt);
            else
            {
                const int top = self->tabArea.bottom, room = self->barArea.top - top - (self->splitArea.bottom - self->splitArea.top);
                if (room > 0)
                {
                    int s = static_cast<int>(static_cast<long long>(pt.y - top) * 1000 / room);
                    self->split = s < 0 ? 0 : s > 1000 ? 1000 : s;
                    self->Layout();
                }
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (self && self->pointing) { self->pointing = false; if (self->pointed) self->pointed(false, 0, 0); }
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (self && self->drag == Drag::None)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&self->imageArea, pt) && self->Scale() > 0)
            {
                ::SetFocus(hwnd);
                self->StartImageDrag(msg == WM_RBUTTONDOWN ? Drag::Zoom : Drag::Pan, pt);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        // Over the image (when Explorer lets it get here): the frames or slices, as in the viewers
        if (self)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // in screen coordinates
            ScreenToClient(hwnd, &pt);
            if ((PtInRect(&self->imageArea, pt) || PtInRect(&self->barArea, pt)) && self->Count() > 1)
            {
                self->Step(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
                return 0;
            }
        }
        break;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (self && (self->drag == Drag::Pan || self->drag == Drag::Zoom))
        {
            self->drag = Drag::None;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONUP:
        if (self && self->drag != Drag::None)
        {
            if (self->drag == Drag::Split)
            {
                DWORD value = static_cast<DWORD>(self->split);
                RegSetKeyValueW(HKEY_CURRENT_USER, Settings, L"Split", REG_DWORD, &value, sizeof(value));
            }
            self->drag = Drag::None;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (self) self->drag = Drag::None;
        return 0;
    case WM_LBUTTONDBLCLK:
        // On the image: its first window again, fitted. Elsewhere, the second click of a pair.
        if (self)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (!PtInRect(&self->imageArea, pt)) return WndProc(hwnd, WM_LBUTTONDOWN, wp, lp);
            if (self->view.imaging)
            {
                self->view.imaging->center = self->view.imaging->center0;
                self->view.imaging->width = self->view.imaging->width0;
                self->windowSet = false;
                self->zoom = 1;
                self->KeepView();
                self->Redraw();
            }
        }
        return 0;
    case WM_NOTIFY:
        if (self && self->list && reinterpret_cast<NMHDR*>(lp)->hwndFrom == self->list &&
            reinterpret_cast<NMHDR*>(lp)->code == LVN_ITEMCHANGED && !self->filling)
        {
            auto change = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK) &&
                (change->uOldState & LVIS_STATEIMAGEMASK))     // not the first check box of a new row
                self->Toggle(change->iItem);
        }
        if (self && self->tree && reinterpret_cast<NMHDR*>(lp)->hwndFrom == self->tree)
        {
            // The tree of a DICOMDIR: a series opened, an item chosen, Enter or a double click
            const UINT code = reinterpret_cast<NMHDR*>(lp)->code;
            if (code == TVN_ITEMEXPANDINGW)
            {
                auto tv = reinterpret_cast<NMTREEVIEWW*>(lp);
                if (tv->action & TVE_EXPAND && tv->itemNew.lParam != -1 && FileOf(tv->itemNew.lParam) == -1)
                    self->ExpandGroup(GroupOf(tv->itemNew.lParam), tv->itemNew.hItem);
            }
            else if (code == TVN_SELCHANGEDW && !self->selecting)
                self->Choose(reinterpret_cast<NMTREEVIEWW*>(lp)->itemNew.lParam);
            else if (code == NM_DBLCLK || code == NM_RETURN)
            {
                TVITEMW item = { TVIF_PARAM };
                item.hItem = TreeView_GetSelection(self->tree);
                if (item.hItem && TreeView_GetItem(self->tree, &item) && item.lParam != -1 && FileOf(item.lParam) != -1)
                {
                    self->Reveal(item.lParam);
                    return TRUE;
                }
            }
        }
        break;
    case WM_PAINT:
    {
        // Painted aside, then copied: no flicker while the window of the image is dragged
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        GetClientRect(hwnd, &c);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        HGDIOBJ old = SelectObject(memory, bitmap);
        if (self) self->Paint(memory);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               memory, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:                         // the header (a read-only edit control)
    case WM_CTLCOLOREDIT:
        if (self)
        {
            SetBkColor(reinterpret_cast<HDC>(wp), self->backColor);
            ::SetTextColor(reinterpret_cast<HDC>(wp), self->textColor);
            return reinterpret_cast<LRESULT>(self->Background());
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
