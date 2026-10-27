// The preview: its layout, and the painting of the image, its overlay and its image bar
#include "Shell.h"

// From the top: the tabs, their content, the bar that moves the split, the image, the image bar
// (frames or slices, the overlay), the status line (the summary, and the window of the image).
// Without an image the content takes their place.
void PreviewHandler::Layout()
{
    RECT c;
    GetClientRect(wnd, &c);
    const int w = c.right, h = c.bottom, line = Px(22), bar = Px(6);
    tabArea = { 0, 0, w, hosted ? 0 : line + Px(4) };
    statusArea = { Px(4), h - line, w - Px(4), h };
    const int top = tabArea.bottom, bottom = h - line > top ? h - line : top;
    if (hosted)
    {
        // The image over all of it, its bar under it (two rows: the times, the places)
        const int image = bottom - 2 * line > top ? bottom - 2 * line : top;
        contentArea = splitArea = {};
        imageArea = { 0, top, w, image };
        barArea = { 0, image, w, bottom };
    }
    else if (!view.pixels.empty())
    {
        const int image = bottom - 2 * line > top ? bottom - 2 * line : top;
        int y = top + static_cast<int>(static_cast<long long>(image - top - bar) * split / 1000);
        if (y > image - bar - line) y = image - bar - line;
        if (y < top + line) y = top + line;
        contentArea = { 0, top, w, y };
        splitArea = { 0, y, w, y + bar };
        imageArea = { 0, y + bar, w, image };
        barArea = { 0, image, w, bottom };
    }
    else
    {
        contentArea = { 0, top, w, bottom };
        splitArea = imageArea = barArea = {};
    }
    const int ch = contentArea.bottom - contentArea.top > 0 ? contentArea.bottom - contentArea.top : 0;
    if (edit) MoveWindow(edit, 0, contentArea.top, w, ch, TRUE);
    if (list) MoveWindow(list, 0, contentArea.top, w, ch, TRUE);
    if (tree) MoveWindow(tree, 0, contentArea.top, w, ch, TRUE);
    InvalidateRect(wnd, nullptr, TRUE);
}

void PreviewHandler::Paint(HDC dc)
{
    RECT c;
    GetClientRect(wnd, &c);
    FillRect(dc, &c, Background());
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
    if (plain)
    {
        ::SetTextColor(dc, Dim(70));
        DrawTextW(dc, view.summary.c_str(), -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        return;
    }

    // The tabs: the chosen one in bold and underlined, the others dimmed. The tree of a DICOMDIR first.
    static const wchar_t* const labels[TabCount] = { L"Columns", L"Header", L"Tags", L"Directory" };
    static const int order[TabCount] = { TabDirectory, TabColumns, TabHeader, TabTags };
    int x = Px(6);
    for (int t : order)
    {
        tabRects[t] = {};
        if ((t == TabDirectory && !tree) || hosted) continue;
        SelectObject(dc, t == tab && boldFont ? boldFont : uiFont);
        // (in the Lister of Total Commander the columns are no columns of its: the values of the file)
        const wchar_t* label = t == TabColumns && listerWindow ? L"Info" : labels[t];
        SIZE ext = {};
        GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &ext);
        tabRects[t] = { x, 0, x + ext.cx + Px(16), tabArea.bottom };
        ::SetTextColor(dc, t == tab ? textColor : Dim(60));
        RECT r = tabRects[t];
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (t == tab)
        {
            RECT u = { tabRects[t].left + Px(4), tabArea.bottom - Px(3), tabRects[t].right - Px(4), tabArea.bottom - Px(1) };
            HBRUSH accent = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
            FillRect(dc, &u, accent);
            DeleteObject(accent);
        }
        x = tabRects[t].right;
    }
    HBRUSH line = CreateSolidBrush(Dim(25));
    RECT under = { 0, tabArea.bottom - Px(1), c.right, tabArea.bottom };
    if (!hosted) FillRect(dc, &under, line);
    if (splitArea.bottom > splitArea.top)
    {
        const int y = (splitArea.top + splitArea.bottom) / 2;
        RECT bar = { 0, y, c.right, y + Px(1) };
        FillRect(dc, &bar, line);
    }
    DeleteObject(line);

    const double scale = Scale(), down = scale * Aspect();     // (along the rows, down the columns)
    if (scale > 0)
    {
        // Fitted into the image area (zoom 1, centered), or the part of it in sight: only the rows and
        // columns in the area are stretched, those from the first one in a DIB of their own. A pixel
        // as its spacing says: `down` high for `scale` wide.
        const double left = (imageArea.left + imageArea.right) / 2.0 - viewX * scale;
        const double top = (imageArea.top + imageArea.bottom) / 2.0 - viewY * down;
        const int x0 = (std::max)(0, static_cast<int>(std::floor((imageArea.left - left) / scale)));
        const int x1 = (std::min)(view.width, static_cast<int>(std::ceil((imageArea.right - left) / scale)));
        const int y0 = (std::max)(0, static_cast<int>(std::floor((imageArea.top - top) / down)));
        const int y1 = (std::min)(view.height, static_cast<int>(std::ceil((imageArea.bottom - top) / down)));
        if (x1 > x0 && y1 > y0)
        {
            const int dx = static_cast<int>(std::lround(left + x0 * scale)), dy = static_cast<int>(std::lround(top + y0 * down));
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = view.width;
            bi.bmiHeader.biHeight = -(y1 - y0);
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            const int saved = SaveDC(dc);
            IntersectClipRect(dc, imageArea.left, imageArea.top, imageArea.right, imageArea.bottom);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, dx, dy, static_cast<int>(std::lround(left + x1 * scale)) - dx,
                          static_cast<int>(std::lround(top + y1 * down)) - dy, x0, 0, x1 - x0, y1 - y0,
                          view.pixels.data() + static_cast<size_t>(y0) * view.width, &bi, DIB_RGB_COLORS, SRCCOPY);
            RestoreDC(dc, saved);
        }
        // The segmentations: their mask (its colors through) and their contours
        if (segments && poster.empty() && (maskBitmap || !outlines.empty()))
        {
            const int saved = SaveDC(dc);
            IntersectClipRect(dc, imageArea.left, imageArea.top, imageArea.right, imageArea.bottom);
            if (maskBitmap)
            {
                HDC src = CreateCompatibleDC(dc);
                HGDIOBJ kept = SelectObject(src, maskBitmap);
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(dc, static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top)),
                           static_cast<int>(std::lround(view.width * scale)), static_cast<int>(std::lround(view.height * down)),
                           src, 0, 0, maskWidth, maskHeight, blend);
                SelectObject(src, kept);
                DeleteDC(src);
            }
            for (const Outline& o : outlines)
            {
                HPEN pen = CreatePen(PS_SOLID, Px(2), o.color);
                HGDIOBJ kept = SelectObject(dc, pen);
                std::vector<POINT> pts;
                for (const auto& p : o.points)
                    pts.push_back({ static_cast<LONG>(std::lround(left + (p[0] + 0.5) * scale)), static_cast<LONG>(std::lround(top + (p[1] + 0.5) * down)) });
                if (o.closed && pts.size() > 2) pts.push_back(pts.front());
                if (pts.size() > 1) Polyline(dc, pts.data(), static_cast<int>(pts.size()));
                SelectObject(dc, kept);
                DeleteObject(pen);
            }
            RestoreDC(dc, saved);
        }
        // The reference lines (dashed, dark yellow) and the crosshair, over the image in sight
        if (hosted && poster.empty() && (!guides.empty() || crosshair))
        {
            const int saved = SaveDC(dc);
            IntersectClipRect(dc, imageArea.left, imageArea.top, imageArea.right, imageArea.bottom);
            auto at = [&](double c, double r) { return POINT{ static_cast<LONG>(std::lround(left + (c + 0.5) * scale)), static_cast<LONG>(std::lround(top + (r + 0.5) * down)) }; };
            LOGBRUSH lb = { BS_SOLID, RGB(214, 170, 0), 0 };
            const DWORD dashes[2] = { static_cast<DWORD>(Px(6)), static_cast<DWORD>(Px(4)) };
            HPEN dashed = ExtCreatePen(PS_GEOMETRIC | PS_USERSTYLE | PS_ENDCAP_FLAT, 1, &lb, 2, dashes);
            HGDIOBJ before = SelectObject(dc, dashed);
            for (const auto& g : guides)
            {
                const POINT a = at(g[0], g[1]), b = at(g[2], g[3]);
                MoveToEx(dc, a.x, a.y, nullptr);
                LineTo(dc, b.x, b.y);
            }
            if (crosshair)
            {
                HPEN cross = CreatePen(PS_SOLID, Px(2), RGB(255, 220, 0));
                SelectObject(dc, cross);
                const POINT p = at(crossColumn, crossRow);
                const int k = Px(8);
                MoveToEx(dc, p.x - k, p.y, nullptr); LineTo(dc, p.x - Px(2), p.y);
                MoveToEx(dc, p.x + Px(2), p.y, nullptr); LineTo(dc, p.x + k, p.y);
                MoveToEx(dc, p.x, p.y - k, nullptr); LineTo(dc, p.x, p.y - Px(2));
                MoveToEx(dc, p.x, p.y + Px(2), nullptr); LineTo(dc, p.x, p.y + k);
                SelectObject(dc, dashed);
                DeleteObject(cross);
            }
            SelectObject(dc, before);
            DeleteObject(dashed);
            RestoreDC(dc, saved);
        }
        // The overlay planes of the image (segmentations too): magenta, the others in colors of their own
        if (segments && poster.empty() && view.imaging && !view.imaging->planes.empty() && view.imaging->planes.size() == static_cast<size_t>(view.width) * view.height)
        {
            static const DWORD colors[4] = { 0xFFFF00FF, 0xFFFFD000, 0xFF00E0FF, 0xFF40FF40 };      // (0xAARRGGBB)
            std::vector<DWORD> argb(view.imaging->planes.size(), 0);
            for (size_t i = 0; i < argb.size(); i++)
                if (const Uint8 p = view.imaging->planes[i]) argb[i] = colors[(p - 1) % 4];
            BITMAPINFO pbi = {};
            pbi.bmiHeader.biSize = sizeof(pbi.bmiHeader);
            pbi.bmiHeader.biWidth = view.width;
            pbi.bmiHeader.biHeight = -view.height;
            pbi.bmiHeader.biPlanes = 1;
            pbi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            if (HBITMAP planes = CreateDIBSection(dc, &pbi, DIB_RGB_COLORS, &bits, nullptr, 0))
            {
                if (bits) memcpy(bits, argb.data(), argb.size() * sizeof(DWORD));
                HDC src = CreateCompatibleDC(dc);
                HGDIOBJ kept = SelectObject(src, planes);
                const int saved = SaveDC(dc);
                IntersectClipRect(dc, imageArea.left, imageArea.top, imageArea.right, imageArea.bottom);
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(dc, static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top)),
                           static_cast<int>(std::lround(view.width * scale)), static_cast<int>(std::lround(view.height * down)),
                           src, 0, 0, view.width, view.height, blend);
                RestoreDC(dc, saved);
                SelectObject(src, kept);
                DeleteDC(src);
                DeleteObject(planes);
            }
        }
        // The overlay, on the part of the image in sight
        RECT shown = { static_cast<LONG>(std::lround(left)), static_cast<LONG>(std::lround(top)),
                       static_cast<LONG>(std::lround(left + view.width * scale)), static_cast<LONG>(std::lround(top + view.height * down)) };
        if (overlay && IntersectRect(&shown, &shown, &imageArea) && shown.right - shown.left >= Px(200) &&
            shown.bottom - shown.top >= Px(150))        // (not on a small image: it would hide it)
            PaintOverlay(dc, shown);
        if (!frameProblem.empty())
        {
            // (in the middle: its height first)
            RECT why = imageArea;
            InflateRect(&why, -Px(12), -Px(12));
            RECT need = why;
            DrawTextW(dc, frameProblem.c_str(), -1, &need, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            why.top = (std::max)(why.top, (imageArea.top + imageArea.bottom - (need.bottom - need.top)) / 2);
            ::SetTextColor(dc, RGB(220, 220, 220));
            DrawTextW(dc, frameProblem.c_str(), -1, &why, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
    }
    PaintBar(dc);

    // The status line: the summary (or a note), and at its right the window of the image
    SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT text = statusArea;
    if (Imaging* im = view.pixels.empty() ? nullptr : view.imaging.get())
    {
        wchar_t window[128], zoomed[32] = L"";
        if (zoom > 1) swprintf_s(zoomed, L"Zoom %.0f%%   ", zoom * 100);
        if (im->mono) swprintf_s(window, L"%sCenter %.0f   Width %.0f", zoomed, im->center, im->width);
        else swprintf_s(window, L"%sBrightness %+.0f   Contrast %.0f%%", zoomed, 127.5 - im->center, 25500.0 / im->width);
        ::SetTextColor(dc, Dim(70));
        SIZE ext = {};
        GetTextExtentPoint32W(dc, window, static_cast<int>(wcslen(window)), &ext);
        if (statusArea.right - statusArea.left > 2 * ext.cx)
        {
            DrawTextW(dc, window, -1, &text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            text.right -= ext.cx + Px(12);
        }
    }
    ::SetTextColor(dc, textColor);
    DrawTextW(dc, (note.empty() || tab != TabColumns ? view.summary : note).c_str(), -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

// The overlay: the corners (Annotate), the window and the zoom at the bottom right, the patient
// directions in the middle of the sides. Light text with a dark shadow, readable on any image.
void PreviewHandler::PaintOverlay(HDC dc, const RECT& r)
{
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    // The corners leave room for the directions at the middle of the top and the bottom
    const int m = Px(6), height = tm.tmHeight, half = (r.right - r.left) / 2 - m - Px(16);
    auto draw = [&](RECT box, const std::wstring& s, UINT align) {
        if (s.empty()) return;
        RECT shadow = box;
        OffsetRect(&shadow, Px(1), Px(1));
        ::SetTextColor(dc, RGB(0, 0, 0));
        DrawTextW(dc, s.c_str(), -1, &shadow, align | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SetTextColor(dc, RGB(235, 235, 235));
        DrawTextW(dc, s.c_str(), -1, &box, align | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    };
    // The lines of a corner, down from the top or up from the bottom, in half the width
    auto corner = [&](const std::wstring& text, bool right, bool bottom) {
        std::vector<std::wstring> lines;
        for (size_t at = 0; !text.empty() && at <= text.size();)
        {
            size_t end = text.find(L'\n', at);
            if (end == std::wstring::npos) end = text.size();
            lines.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        int y = bottom ? r.bottom - m - static_cast<int>(lines.size()) * height : r.top + m;
        for (const auto& line : lines)
        {
            draw({ right ? r.right - m - half : r.left + m, y, right ? r.right - m : r.left + m + half, y + height }, line,
                 right ? DT_RIGHT : DT_LEFT);
            y += height;
        }
    };
    std::wstring topLeft = view.corners[0];
    if (view.frames > 1)
        topLeft += (topLeft.empty() ? L"Frame " : L"\nFrame ") + std::to_wstring(view.frame + 1) + L" / " + std::to_wstring(view.frames);
    corner(topLeft, false, false);
    corner(view.corners[1], true, false);
    corner(view.corners[2], false, true);
    if (Imaging* im = view.imaging.get())
    {
        wchar_t text[96];
        if (im->mono) swprintf_s(text, L"C %.0f  W %.0f", im->center, im->width);
        else swprintf_s(text, L"Brightness %+.0f  Contrast %.0f%%", 127.5 - im->center, 25500.0 / im->width);
        std::wstring bottomRight = text;
        if (zoom > 1)
        {
            swprintf_s(text, L"\nZoom %.0f%%", zoom * 100);
            bottomRight += text;
        }
        corner(bottomRight, true, true);
    }
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    draw({ r.left + m, cy - height / 2, cx, cy + height }, view.sides[0], DT_LEFT);
    draw({ r.left, r.top + m, r.right, r.top + m + height }, view.sides[1], DT_CENTER);
    draw({ cx, cy - height / 2, r.right - m, cy + height }, view.sides[2], DT_RIGHT);
    draw({ r.left, r.bottom - m - height, r.right, r.bottom - m }, view.sides[3], DT_CENTER);
    RestoreDC(dc, saved);
}

// The image bar, two rows: the times (play / pause, the frames of a multi-frame file or the images of a
// position) with the check box of the overlay at the right; the places (the jumps through the planes
// or the series, or the files of a folder) with that of the segmentations. Each one gray when there is nothing to go through.
void PreviewHandler::PaintBar(HDC dc)
{
    playRect = jumpRect = trackRect = spaceTrackRect = overlayRect = segmentsRect = {};
    if (barArea.bottom <= barArea.top) return;
    SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
    const int middle = (barArea.top + barArea.bottom) / 2;
    const RECT rows[2] = { { barArea.left, barArea.top, barArea.right, middle }, { barArea.left, middle, barArea.right, barArea.bottom } };
    HBRUSH accent = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT)), ink = CreateSolidBrush(textColor);
    HBRUSH groove = CreateSolidBrush(Dim(30)), border = CreateSolidBrush(Dim(60)), off = CreateSolidBrush(Dim(35));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN)), oldBrush = SelectObject(dc, ink);
    SIZE ext = {};

    // The overlay, at the right of the first row, and under it the segmentations when the image has any
    // (hosted: the check boxes of the explorer)
    if (!hosted)
    {
        static const wchar_t overlayLabel[] = L"Overlay", segmentsLabel[] = L"Segmentations";
        SIZE wide = {};
        GetTextExtentPoint32W(dc, segmentsLabel, ARRAYSIZE(segmentsLabel) - 1, &wide);
        const int box = Px(13), right = barArea.right - Px(6), left = right - wide.cx - Px(6) - box;
        auto check = [&](const RECT& r, const wchar_t* label, bool on, RECT& area) {
            const int cy = (r.top + r.bottom) / 2;
            area = { left, r.top, right, r.bottom };
            RECT square = { left, cy - box / 2, left + box, cy - box / 2 + box };
            if (on)
            {
                FillRect(dc, &square, accent);
                HPEN pen = CreatePen(PS_SOLID, Px(2), RGB(255, 255, 255));
                SelectObject(dc, pen);
                POINT mark[3] = { { square.left + box * 22 / 100, cy }, { square.left + box * 42 / 100, cy + box * 22 / 100 },
                                  { square.left + box * 78 / 100, cy - box * 22 / 100 } };
                Polyline(dc, mark, 3);
                SelectObject(dc, GetStockObject(NULL_PEN));
                DeleteObject(pen);
            }
            else FrameRect(dc, &square, border);
            ::SetTextColor(dc, textColor);
            RECT text = { square.right + Px(6), r.top, right, r.bottom };
            DrawTextW(dc, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        };
        check(rows[0], overlayLabel, overlay, overlayRect);
        if (HasSegments()) check(rows[1], segmentsLabel, segments, segmentsRect);
    }
    const int rightEdge = hosted ? barArea.right - Px(6) : overlayRect.left - Px(14);
    const bool any = Count() > 0 && (Position() >= 0 || !poster.empty());

    // A row: its button, its slider at `at` of `n`, and where it is
    auto row = [&](int which, bool active, int at, int n, const std::wstring& where, RECT& button, RECT& track) {
        const RECT& r = rows[which];
        const int cy = (r.top + r.bottom) / 2;
        int left = r.left + Px(6), right = rightEdge;
        const RECT b = { left, r.top, left + Px(24), r.bottom };
        if (active) button = b;
        const int px = (b.left + b.right) / 2, s = Px(10);
        if (which == 0)
        {
            SelectObject(dc, active ? ink : off);
            if (playing)
            {
                RECT a1 = { px - s / 2, cy - s / 2, px - s / 2 + s * 35 / 100, cy - s / 2 + s };
                RECT a2 = { px + s / 2 - s * 35 / 100, cy - s / 2, px + s / 2, cy - s / 2 + s };
                FillRect(dc, &a1, ink);
                FillRect(dc, &a2, ink);
            }
            else
            {
                POINT triangle[3] = { { px - s / 2 + 1, cy - s / 2 }, { px - s / 2 + 1, cy + s / 2 }, { px + s / 2, cy } };
                Polygon(dc, triangle, 3);
            }
        }
        else
        {
            // Three planes one behind the other (the front one in the accent color while jumping)
            HPEN edge = CreatePen(PS_SOLID, 1, active ? textColor : Dim(35));
            SelectObject(dc, edge);
            for (int k = 2; k >= 0; k--)
            {
                const int dy = (1 - k) * Px(4), w = Px(7), h = Px(2), skew = Px(4);     // (k 2 the back one, at the top)
                POINT sheet[4] = { { px - w, cy + dy + h }, { px - w + skew, cy + dy - h }, { px + w, cy + dy - h }, { px + w - skew, cy + dy + h } };
                SelectObject(dc, k == 0 ? (jumping ? accent : (active ? ink : off)) : GetStockObject(active ? WHITE_BRUSH : LTGRAY_BRUSH));
                Polygon(dc, sheet, 4);
            }
            SelectObject(dc, GetStockObject(NULL_PEN));
            DeleteObject(edge);
        }
        left = b.right + Px(8);
        GetTextExtentPoint32W(dc, where.c_str(), static_cast<int>(where.size()), &ext);
        const int textWidth = (std::max)(static_cast<int>(ext.cx), Px(110));
        RECT w = { right - textWidth, r.top, right, r.bottom };
        if (w.left > left + Px(40))
        {
            ::SetTextColor(dc, active ? Dim(70) : Dim(40));
            DrawTextW(dc, where.c_str(), -1, &w, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            right = w.left - Px(12);
        }
        if (right - left > Px(30))
        {
            const int x = n > 1 ? left + static_cast<int>(static_cast<long long>(right - left) * (std::max)(0, at) / (n - 1)) : left;
            RECT groove1 = { left, cy - Px(1), right, cy - Px(1) + Px(2) }, done = { left, groove1.top, x, groove1.bottom };
            FillRect(dc, &groove1, groove);
            if (active)
            {
                track = { left, r.top, right, r.bottom };
                FillRect(dc, &done, accent);
            }
            SelectObject(dc, active ? accent : off);
            const int k = Px(5);
            Ellipse(dc, x - k, cy - k, x + k + 1, cy + k + 1);
        }
    };
    wchar_t where[96];
    const int nt = any ? TimeCount() : 0, ns = any ? SpaceCount() : 0;
    const int at = any ? TimeIndex() : 0, as = any ? SpaceIndex() : 0;
    swprintf_s(where, OverFrames() ? L"Frame %d / %d" : L"Time %d / %d", at + 1, (std::max)(1, nt));
    row(0, nt > 1, at, nt, where, playRect, trackRect);
    if (!poster.empty()) swprintf_s(where, L"Thumbnail   %d files", Count());
    else swprintf_s(where, L"%s %d / %d", spaceName, as + 1, (std::max)(1, ns));
    row(1, ns > 1, as, ns, where, jumpRect, spaceTrackRect);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    for (HBRUSH b : { accent, ink, groove, border, off }) DeleteObject(b);
}
