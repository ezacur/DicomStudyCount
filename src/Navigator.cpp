// The navigator of the explorer: the images as planes where they are in the patient, in 3-D
#include "Shell.h"

// ----- the navigator (DCMexplorer_navigator.m): a window of its own with the images as planes where
// they are in the patient, in 3-D (orthographic); those of the node chosen in red, and the image itself
// on its plane when they are one plane. Drag: rotate; right drag (or the wheel): zoom; Shift+drag (or the middle button): move; double click: again.

Navigator::~Navigator() { for (HBITMAP b : { bitmap, liveBitmap }) if (b) DeleteObject(b); if (font) DeleteObject(font); }

bool Navigator::Create(HWND parent)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = L"Explorer4DicomsNavigator";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    wnd = CreateWindowExW(WS_EX_CLIENTEDGE, wc.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, g_module, this);
    return wnd != nullptr;
}

bool Navigator::Visible() const { return wnd && shown; }

void Navigator::Show(bool on) { shown = on; if (wnd) ShowWindow(wnd, on ? SW_SHOWNA : SW_HIDE); }

void Navigator::Move(const RECT& r) { if (wnd) MoveWindow(wnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE); }

// What it draws: planes (hot: of the node chosen), and a picture on one of them (-1: none). Fit: the
// view moved to these planes (another study), its zoom and move undone; the rotation stays.
void Navigator::Set(std::vector<NavPlane> p, std::vector<char> h, int on, Picture picture, std::wstring text, std::wstring more, bool fit)
{
    planes = std::move(p);
    corners.clear();
    hover = -1;
    marks.clear();
    guides.clear();
    contours.clear();
    if (liveBitmap) { DeleteObject(liveBitmap); liveBitmap = nullptr; }
    live = -1;
    hoverText.clear();
    hot = std::move(h);
    textured = on;
    caption = std::move(text);
    detail = std::move(more);
    if (bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
    texture = std::move(picture);
    if (textured >= 0 && texture.width > 0)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = texture.width;
        bi.bmiHeader.biHeight = -texture.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) memcpy(bits, texture.px.data(), texture.px.size() * sizeof(DWORD));
    }
    if (fit)
    {
        zoom = 1;
        panX = panY = 0;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (const auto& q : planes)
            for (const auto& c : q.p)
                for (int k = 0; k < 3; k++) { lo[k] = (std::min)(lo[k], c[k]); hi[k] = (std::max)(hi[k], c[k]); }
        radius = 1;
        if (!planes.empty())
        {
            double d = 0;
            for (int k = 0; k < 3; k++) { center[k] = (lo[k] + hi[k]) / 2; d += (hi[k] - lo[k]) * (hi[k] - lo[k]); }
            radius = (std::max)(1.0, std::sqrt(d) / 2);
        }
    }
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

void Navigator::Clear() { Set({}, {}, -1, Picture(), L"", L"", true); }

// Lines in the patient (reference lines: where the planes cut the one shown), dashed, and a point
void Navigator::SetGuides(std::vector<std::array<float, 6>> lines)
{
    guides = std::move(lines);
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

void Navigator::SetPoint(bool on, const double* p)
{
    if (!on && !pointOn) return;
    pointOn = on;
    if (on && p) for (int k = 0; k < 3; k++) point[k] = p[k];
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

void Navigator::SetContours(std::vector<Line3> lines)
{
    contours = std::move(lines);
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

// The image the cine or the jumps show, on its plane (-1: none)
void Navigator::SetLive(int i, int w, int h, const std::vector<DWORD>* px)
{
    if (liveBitmap) { DeleteObject(liveBitmap); liveBitmap = nullptr; }
    live = -1;
    if (i >= 0 && i < static_cast<int>(planes.size()) && px && w > 0 && h > 0 && px->size() >= static_cast<size_t>(w) * h)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        liveBitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (liveBitmap && bits)
        {
            memcpy(bits, px->data(), static_cast<size_t>(w) * h * sizeof(DWORD));
            live = i;
            liveWidth = w;
            liveHeight = h;
        }
    }
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

void Navigator::SetHoverText(std::wstring text) { hoverText = std::move(text); if (wnd) InvalidateRect(wnd, nullptr, FALSE); }

// Planes framed in black too (those of the node under the mouse in the tree)
void Navigator::SetMarks(std::vector<int> which)
{
    if (which == marks) return;
    marks = std::move(which);
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

int Navigator::Px(int n) const { const UINT dpi = GetDpiForWindow(wnd); return MulDiv(n, dpi ? dpi : 96, 96); }

// Where a point of the patient is in the window: viewmtx( az , el ) of MATLAB, its up up
void Navigator::Axes(double r[3][3]) const
{
    const double a = azimuth * 3.14159265358979 / 180, e = elevation * 3.14159265358979 / 180;
    const double m[3][3] = { { cos(a), sin(a), 0 }, { -sin(e) * sin(a), sin(e) * cos(a), cos(e) }, { cos(e) * sin(a), -cos(e) * cos(a), sin(e) } };
    memcpy(r, m, sizeof(m));
}

void Navigator::Figure(HDC dc, const double r[3][3], int w, int h)
{
    struct Block { double c[3], half[3]; COLORREF color; };
    static const Block blocks[] = {
        { { 0, 0, 8.1 }, { 1.0, 1.1, 1.2 }, RGB(236, 196, 164) },        // head
        { { 0, -1.3, 7.9 }, { 0.22, 0.3, 0.35 }, RGB(214, 160, 128) },   // nose
        { { 0, 0, 4.1 }, { 2.0, 1.0, 2.6 }, RGB(90, 140, 210) },         // body
        { { 2.65, 0, 4.4 }, { 0.6, 0.6, 2.3 }, RGB(90, 140, 210) },      // left arm
        { { -2.65, 0, 4.4 }, { 0.6, 0.6, 2.3 }, RGB(90, 140, 210) },     // right arm
        { { 0.95, 0, -1.6 }, { 0.8, 0.8, 3.0 }, RGB(70, 80, 110) },      // left leg
        { { -0.95, 0, -1.6 }, { 0.8, 0.8, 3.0 }, RGB(70, 80, 110) },     // right leg
    };
    const double middle = 2.2;              // (its height from -4.6 to 9.3)
    // (14 high, and as much across when it lies: in a square of a third of the panel, at most 120 px)
    const int size = (std::max)(Px(48), (std::min)(Px(120), (std::min)(w, h) / 3));
    const double k = size / 14.0;
    const POINT o = { Px(8) + size / 2, h - Px(8) - size / 2 };
    auto at = [&](const double v[3]) {
        const double p[3] = { v[0], v[1], v[2] - middle };
        return POINT{ o.x + static_cast<LONG>(std::lround((r[0][0] * p[0] + r[0][1] * p[1] + r[0][2] * p[2]) * k)),
                      o.y - static_cast<LONG>(std::lround((r[1][0] * p[0] + r[1][1] * p[1] + r[1][2] * p[2]) * k)) };
    };
    struct Drawn { Side side; double depth; COLORREF color; };
    std::vector<Drawn> drawn;
    for (const Block& b : blocks)
        for (int axis = 0; axis < 3; axis++)
            for (int sign : { -1, 1 })
            {
                double n[3] = {};
                n[axis] = sign;
                const double facing = r[2][0] * n[0] + r[2][1] * n[1] + r[2][2] * n[2];
                if (facing <= 1e-6) continue;       // (its back)
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                Drawn d = {};
                double centre[3] = { b.c[0], b.c[1], b.c[2] };
                centre[axis] += sign * b.half[axis];
                const int square[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
                for (int c = 0; c < 4; c++)
                {
                    double p[3] = { centre[0], centre[1], centre[2] };
                    p[u] += square[c][0] * b.half[u];
                    p[v] += square[c][1] * b.half[v];
                    d.side.p[c] = at(p);
                }
                memcpy(d.side.normal, n, sizeof(n));
                d.depth = r[2][0] * centre[0] + r[2][1] * centre[1] + r[2][2] * (centre[2] - middle);
                const double light = 0.55 + 0.45 * facing;
                d.color = RGB(static_cast<int>(GetRValue(b.color) * light), static_cast<int>(GetGValue(b.color) * light),
                              static_cast<int>(GetBValue(b.color) * light));
                drawn.push_back(d);
            }
    std::sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.depth < b.depth; });
    figure.clear();
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    for (const Drawn& d : drawn)
    {
        HBRUSH brush = CreateSolidBrush(d.color);
        HGDIOBJ old = SelectObject(dc, brush);
        Polygon(dc, d.side.p, 4);
        SelectObject(dc, old);
        DeleteObject(brush);
        figure.push_back(d.side);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    // Its left and its right, at its hands
    SetTextColor(dc, RGB(150, 0, 0));
    for (int s : { 1, -1 })
    {
        const double hand[3] = { s * 3.9, 0, 2.0 };
        const POINT p = at(hand);
        TextOutW(dc, p.x - Px(4), p.y - Px(8), s > 0 ? L"L" : L"R", 1);
    }
}

// The side of the figure under a point of the window (nullptr: none)
const Navigator::Side* Navigator::SideAt(POINT pt) const
{
    for (auto it = figure.rbegin(); it != figure.rend(); ++it)
    {
        bool in = false;
        for (int i = 0, j = 3; i < 4; j = i++)
        {
            const POINT& a = it->p[i];
            const POINT& b = it->p[j];
            if ((a.y > pt.y) != (b.y > pt.y) && pt.x < (b.x - a.x) * static_cast<double>(pt.y - a.y) / (b.y - a.y) + a.x) in = !in;
        }
        if (in) return &*it;
    }
    return nullptr;
}

// The view from where a side faces: from the front, from the left, from above...
void Navigator::LookFrom(const double n[3])
{
    elevation = std::asin((std::max)(-1.0, (std::min)(1.0, n[2]))) * 180 / 3.14159265358979;
    if (std::fabs(n[2]) < 0.999) azimuth = std::atan2(n[0], -n[1]) * 180 / 3.14159265358979;
    else azimuth = 0;
    InvalidateRect(wnd, nullptr, FALSE);
}

// The plane whose outline is under a point (the nearest one, a few pixels away at most), or whose
// image is: -1 if none (or the figure is there)
int Navigator::PlaneAt(POINT pt) const
{
    if (SideAt(pt)) return -1;
    const size_t n = (std::min)(planes.size(), corners.size() / 4);
    double best = Px(5) + 0.5;
    int found = -1;
    for (size_t i = 0; i < n; i++)
    {
        const POINT* c = &corners[i * 4];
        static const int edges[4][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 } };
        for (const auto& e : edges)
        {
            const double ax = c[e[0]].x, ay = c[e[0]].y, bx = c[e[1]].x - ax, by = c[e[1]].y - ay;
            const double len = bx * bx + by * by;
            double t = len > 0 ? ((pt.x - ax) * bx + (pt.y - ay) * by) / len : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const double d = std::hypot(pt.x - (ax + t * bx), pt.y - (ay + t * by));
            if (d < best) { best = d; found = static_cast<int>(i); }
        }
    }
    if (found < 0 && bitmap && textured >= 0 && static_cast<size_t>(textured) < n)
    {
        // Inside the image (a parallelogram: the points by the two sides from its first corner)
        const POINT* c = &corners[textured * 4];
        const double ux = c[1].x - c[0].x, uy = c[1].y - c[0].y, vx = c[2].x - c[0].x, vy = c[2].y - c[0].y;
        const double det = ux * vy - uy * vx, px = pt.x - c[0].x, py = pt.y - c[0].y;
        if (std::fabs(det) > 1)
        {
            const double a = (px * vy - py * vx) / det, b = (ux * py - uy * px) / det;
            if (a >= 0 && a <= 1 && b >= 0 && b <= 1) found = textured;
        }
    }
    return found;
}

// The line of sight under a point of the view: the point of the patient there on the plane of the
// screen, and the direction toward the viewer
void Navigator::Aim(POINT pt)
{
    if (!aimed) return;
    RECT c;
    GetClientRect(wnd, &c);
    if (c.right <= 0 || c.bottom <= 0 || planes.empty()) { aimed(false, nullptr, nullptr); return; }
    double r[3][3];
    Axes(r);
    const double scale = (std::min)(c.right, c.bottom) * 0.5 / radius * zoom;
    const double x = (pt.x - c.right / 2.0 - panX) / scale, y = -(pt.y - c.bottom / 2.0 - panY) / scale;
    double from[3], toward[3];
    for (int k = 0; k < 3; k++) { from[k] = center[k] + x * r[0][k] + y * r[1][k]; toward[k] = r[2][k]; }
    aimed(true, from, toward);
}

void Navigator::Hover(int i)
{
    if (i == hover) return;
    hover = i;
    if (i < 0) hoverText.clear();
    InvalidateRect(wnd, nullptr, FALSE);
    if (hovered) hovered(i);
}

void Navigator::Paint(HDC target)
{
    RECT c;
    GetClientRect(wnd, &c);
    const int w = c.right, h = c.bottom;
    if (w <= 0 || h <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP canvas = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldCanvas = SelectObject(dc, canvas);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    const UINT dpi = GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96;
    if (!font || fontDpi != dpi)
    {
        if (font) DeleteObject(font);
        fontDpi = dpi;
        font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    }
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    double r[3][3];
    Axes(r);
    const double scale = (std::min)(w, h) * 0.5 / radius * zoom;
    auto project = [&](const float* p) {
        const double v[3] = { p[0] - center[0], p[1] - center[1], p[2] - center[2] };
        const double x = r[0][0] * v[0] + r[0][1] * v[1] + r[0][2] * v[2];
        const double y = r[1][0] * v[0] + r[1][1] * v[1] + r[1][2] * v[2];
        return POINT{ static_cast<LONG>(std::lround(w / 2.0 + panX + x * scale)), static_cast<LONG>(std::lround(h / 2.0 + panY - y * scale)) };
    };
    // Where the corners of the planes are (kept, for the mouse over them)
    corners.resize(planes.size() * 4);
    for (size_t i = 0; i < planes.size(); i++)
        for (int k = 0; k < 4; k++) corners[i * 4 + k] = project(planes[i].p[k]);
    // The outlines, the others first, in one call for each color
    auto outlines = [&](int which, COLORREF color, int width) {
        std::vector<POINT> points;
        std::vector<DWORD> counts;
        for (size_t i = 0; i < planes.size(); i++)
        {
            if (which == 2 ? static_cast<int>(i) != hover && static_cast<int>(i) != live && !std::binary_search(marks.begin(), marks.end(), static_cast<int>(i))
                           : (hot[i] != 0) != (which != 0)) continue;
            for (int k : { 0, 1, 3, 2, 0 }) points.push_back(corners[i * 4 + k]);
            counts.push_back(5);
        }
        if (counts.empty()) return;
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ old = SelectObject(dc, pen);
        PolyPolyline(dc, points.data(), counts.data(), static_cast<DWORD>(counts.size()));
        SelectObject(dc, old);
        DeleteObject(pen);
    };
    outlines(0, RGB(200, 200, 200), 1);
    if (bitmap && textured >= 0 && textured < static_cast<int>(planes.size()))
    {
        const POINT* at = &corners[textured * 4];
        POINT to[3] = { at[0], at[1], at[2] };
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(src, bitmap);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        PlgBlt(dc, to, src, 0, 0, texture.width, texture.height, nullptr, 0, 0);
        SelectObject(src, old);
        DeleteDC(src);
    }
    if (liveBitmap && live >= 0 && live < static_cast<int>(planes.size()))
    {
        const POINT* at = &corners[live * 4];
        POINT to[3] = { at[0], at[1], at[2] };
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(src, liveBitmap);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        PlgBlt(dc, to, src, 0, 0, liveWidth, liveHeight, nullptr, 0, 0);
        SelectObject(src, old);
        DeleteDC(src);
    }
    outlines(1, RGB(220, 0, 0), Px(2));
    outlines(2, HoverColor, Px(2));             // (the one under the mouse)
    // The contours of the segmentations
    for (const Line3& l : contours)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, l.color);
        HGDIOBJ old = SelectObject(dc, pen);
        std::vector<POINT> pts;
        for (const auto& p : l.points) pts.push_back(project(p.data()));
        if (l.closed && pts.size() > 2) pts.push_back(pts.front());
        if (pts.size() > 1) Polyline(dc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    // The reference lines, dashed, and the point of the mouse in the image
    if (!guides.empty() || pointOn)
    {
        LOGBRUSH lb = { BS_SOLID, RGB(200, 155, 0), 0 };
        const DWORD dashes[2] = { static_cast<DWORD>(Px(5)), static_cast<DWORD>(Px(3)) };
        HPEN dashed = ExtCreatePen(PS_GEOMETRIC | PS_USERSTYLE | PS_ENDCAP_FLAT, 1, &lb, 2, dashes);
        HGDIOBJ old = SelectObject(dc, dashed);
        for (const auto& g : guides)
        {
            const POINT a = project(&g[0]), b = project(&g[3]);
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
        SelectObject(dc, old);
        DeleteObject(dashed);
        if (pointOn)
        {
            const float pf[3] = { static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2]) };
            const POINT q = project(pf);
            HBRUSH dot = CreateSolidBrush(RGB(255, 210, 0));
            HPEN rim = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, rim);
            const int k = Px(4);
            Ellipse(dc, q.x - k, q.y - k, q.x + k + 1, q.y + k + 1);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(dot);
            DeleteObject(rim);
        }
    }
    // The patient, at the bottom left, turned as the planes are: a click on a side of it, the view
    // from that side
    Figure(dc, r, w, h);
    // What it is, at the top; how to move it, at the bottom right
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT top = { Px(8), Px(6), w - Px(8), Px(48) };
    const std::wstring head = planes.empty() ? (caption.empty() ? L"Choose a node of the tree" : caption + L"\n" + (detail.empty() ? L"No image here has a position in the patient" : detail))
                                             : caption + (detail.empty() ? L"" : L"\n" + detail);
    RECT fill = top;
    DrawTextW(dc, head.c_str(), -1, &fill, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
    fill.right = (std::min)(fill.right, top.right);
    InflateRect(&fill, Px(3), Px(1));
    FillRect(dc, &fill, GetSysColorBrush(COLOR_WINDOW));
    DrawTextW(dc, head.c_str(), -1, &top, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (hover >= 0 && !hoverText.empty())
    {
        RECT under = { Px(8), fill.bottom + Px(2), w - Px(8), fill.bottom + Px(22) };
        RECT back = under;
        DrawTextW(dc, hoverText.c_str(), -1, &back, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        back.right = (std::min)(back.right, under.right);
        InflateRect(&back, Px(3), Px(1));
        FillRect(dc, &back, GetSysColorBrush(COLOR_WINDOW));
        SetTextColor(dc, HoverColor);
        DrawTextW(dc, hoverText.c_str(), -1, &under, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldCanvas);
    DeleteObject(canvas);
    DeleteDC(dc);
}

LRESULT CALLBACK Navigator::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<Navigator*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        self = static_cast<Navigator*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->wnd = hwnd;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) self->Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (!self) break;
        // Left: rotate (with Shift: move); right: zoom (up: in); middle: move
        self->moving = msg == WM_MBUTTONDOWN || (msg == WM_LBUTTONDOWN && (wp & MK_SHIFT));
        self->rotating = msg == WM_LBUTTONDOWN && !self->moving;
        self->zooming = msg == WM_RBUTTONDOWN;
        self->last = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetFocus(hwnd);
        self->down = self->last;
        self->Hover(-1);
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (self && (self->rotating || self->moving || self->zooming))
        {
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), dx = x - self->last.x, dy = y - self->last.y;
            self->last = { x, y };
            if (self->rotating)
            {
                self->azimuth = std::fmod(self->azimuth - dx * 0.5, 360.0);
                self->elevation = (std::max)(-90.0, (std::min)(90.0, self->elevation + dy * 0.5));
            }
            else if (self->zooming) self->zoom = (std::max)(0.05, (std::min)(200.0, self->zoom * std::pow(1.01, -dy)));
            else { self->panX += dx; self->panY += dy; }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (self)
        {
            if (!self->tracking)
            {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
                self->tracking = TrackMouseEvent(&t) != FALSE;
            }
            self->Hover(self->PlaneAt({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }));
            self->Aim({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        }
        return 0;
    case WM_MOUSELEAVE:
        if (self) { self->tracking = false; self->Hover(-1); if (self->aimed) self->aimed(false, nullptr, nullptr); }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (self && (self->rotating || self->moving || self->zooming))
        {
            ReleaseCapture();
            // A click (not a drag) on the figure: the view from that side of it
            const POINT up = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const Side* side = self->SideAt(self->down);
            if (msg == WM_LBUTTONUP && side && std::abs(up.x - self->down.x) + std::abs(up.y - self->down.y) <= self->Px(3))
            {
                self->LookFrom(side->normal);
            }
        }
        return 0;
    case WM_SETCURSOR:
        if (self && LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursor(nullptr, self->SideAt(pt) ? IDC_HAND : IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_CAPTURECHANGED:
        if (self) self->rotating = self->moving = self->zooming = false;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (!self) break;
        // On a plane: its image chosen in the tree; elsewhere, the view as at first
        if (self->picked)
        {
            const int i = self->PlaneAt({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (i >= 0) { self->picked(i); return 0; }
        }
        self->azimuth = -37.5;
        self->elevation = 30;
        self->zoom = 1;
        self->panX = self->panY = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (!self) break;
        self->zoom = (std::max)(0.05, (std::min)(200.0, self->zoom * std::pow(1.15, GET_WHEEL_DELTA_WPARAM(wp) / 120.0)));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        if (self) self->wnd = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
