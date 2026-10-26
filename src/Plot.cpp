// The plot of an attribute along the files of a node, under the full info of the explorer
#include "Shell.h"

// ----- the plot of an attribute along the files of a node, under the full info: its value file after file
// (numbers: a line for each one of them, up to 3; text: its values as rows), the image shown marked; the
// mouse over it says which file, a click chooses it in the tree

Plot::~Plot() { if (font) DeleteObject(font); }

bool Plot::Create(HWND parent)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = L"DicomStudyCountPlot";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    wnd = CreateWindowExW(WS_EX_CLIENTEDGE, wc.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, g_module, this);
    return wnd != nullptr;
}

bool Plot::Visible() const { return wnd && shown; }

void Plot::Show(bool on) { shown = on; if (wnd) ShowWindow(wnd, on ? SW_SHOWNA : SW_HIDE); }

void Plot::Move(const RECT& r) { if (wnd) MoveWindow(wnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE); }

// The values, one for each file (empty: it has none), their names, and the one shown (-1: none)
void Plot::Set(std::wstring what, std::vector<std::wstring> values, std::vector<std::wstring> names, int current)
{
    title = std::move(what);
    text = std::move(values);
    files = std::move(names);
    now = current;
    hover = -1;
    numbers.assign(text.size(), {});
    categories.clear();
    category.assign(text.size(), -1);
    parts = 0;
    numeric = true;
    for (size_t i = 0; i < text.size() && numeric; i++)
    {
        if (text[i].empty()) continue;
        std::vector<double> v;
        size_t from = 0;
        while (from <= text[i].size())
        {
            size_t to = text[i].find(L'\\', from);
            if (to == std::wstring::npos) to = text[i].size();
            const std::wstring part = text[i].substr(from, to - from);
            wchar_t* end = nullptr;
            const double x = wcstod(part.c_str(), &end);
            while (end && *end == L' ') end++;
            if (part.empty() || !end || *end || !std::isfinite(x)) { numeric = false; break; }
            v.push_back(x);
            from = to + 1;
        }
        if (v.size() > 3) v.resize(3);
        parts = (std::max)(parts, v.size());
        numbers[i] = std::move(v);
    }
    if (!numeric)
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i].empty()) continue;
            auto it = std::find(categories.begin(), categories.end(), text[i]);
            category[i] = static_cast<int>(it - categories.begin());
            if (it == categories.end()) categories.push_back(text[i]);
        }
    if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

void Plot::SetCurrent(int i) { if (i != now) { now = i; if (wnd) InvalidateRect(wnd, nullptr, FALSE); } }

int Plot::Px(int n) const { const UINT dpi = GetDpiForWindow(wnd); return MulDiv(n, dpi ? dpi : 96, 96); }

// The range of the values, and where a file and a value are in the window
void Plot::Range(double& lo, double& hi) const
{
    lo = 1e300;
    hi = -1e300;
    if (numeric) { for (const auto& v : numbers) for (double x : v) { lo = (std::min)(lo, x); hi = (std::max)(hi, x); } }
    else { lo = 0; hi = categories.empty() ? 1 : static_cast<double>(categories.size() - 1); }
    if (lo > hi) { lo = 0; hi = 1; }
    if (hi - lo < 1e-12) { lo -= 1; hi += 1; }
    const double m = (hi - lo) * 0.06;
    lo -= m;
    hi += m;
}

int Plot::X(int i) const
{
    const int n = static_cast<int>(text.size());
    return n <= 1 ? (area.left + area.right) / 2 : area.left + static_cast<int>(static_cast<long long>(area.right - area.left) * i / (n - 1));
}

int Plot::Y(double v, double lo, double hi) const { return area.bottom - static_cast<int>(std::lround((v - lo) / (hi - lo) * (area.bottom - area.top))); }

int Plot::At(int x) const     // (the file nearest a point)
{
    const int n = static_cast<int>(text.size());
    if (n == 0 || area.right <= area.left) return -1;
    const double t = static_cast<double>(x - area.left) / (area.right - area.left);
    return (std::max)(0, (std::min)(n - 1, static_cast<int>(std::lround(t * (n - 1)))));
}

void Plot::Paint(HDC target)
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
    const int n = static_cast<int>(text.size());
    // Its title, the file under the mouse, and the x that closes it
    closeBox = { w - Px(22), Px(2), w - Px(4), Px(20) };
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(dc, L"\u00D7", -1, &closeBox, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    std::wstring head = title;
    if (hover >= 0 && hover < n)
        head += L"     #" + std::to_wstring(hover + 1) + L":  " + (text[hover].empty() ? L"(none)" : text[hover]) +
                (hover < static_cast<int>(files.size()) ? L"   " + files[hover] : L"");
    // (several numbers: which line is which)
    int legend = closeBox.left - Px(4);
    if (numeric && parts > 1)
    {
        static const COLORREF ink[3] = { RGB(0, 90, 230), RGB(220, 60, 60), RGB(0, 150, 80) };
        static const wchar_t* const names[3] = { L"1st", L"2nd", L"3rd" };
        for (int k = static_cast<int>(parts) - 1; k >= 0; k--)
        {
            SIZE ext = {};
            GetTextExtentPoint32W(dc, names[k], 3, &ext);
            RECT r = { legend - ext.cx, Px(3), legend, Px(20) };
            SetTextColor(dc, ink[k]);
            DrawTextW(dc, names[k], 3, &r, DT_RIGHT | DT_SINGLELINE);
            legend -= ext.cx + Px(8);
        }
    }
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT top = { Px(6), Px(3), legend - Px(4), Px(20) };
    DrawTextW(dc, head.c_str(), -1, &top, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    area = { Px(numeric ? 64 : 120), Px(26), w - Px(12), h - Px(20) };
    if (n > 0 && area.right > area.left + Px(20) && area.bottom > area.top + Px(20))
    {
        double lo, hi;
        Range(lo, hi);
        HPEN axis = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HGDIOBJ oldPen = SelectObject(dc, axis);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        // The rows of the values: 5 numbers, or the texts
        std::vector<std::pair<double, std::wstring>> ticks;
        if (numeric)
            for (int k = 0; k <= 4; k++)
            {
                double v = lo + (hi - lo) * (0.06 / 1.12 + k * (1 - 0.12 / 1.12) / 4);
                if (std::fabs(v) < (hi - lo) * 1e-9) v = 0;     // (not -1.4e-14)
                wchar_t b[32];
                swprintf_s(b, L"%.4g", v);
                ticks.push_back({ v, b });
            }
        else
        {
            const size_t step = categories.size() > 8 ? (categories.size() + 7) / 8 : 1;
            for (size_t k = 0; k < categories.size(); k += step) ticks.push_back({ static_cast<double>(k), categories[k] });
        }
        for (const auto& [v, label] : ticks)
        {
            const int y = Y(v, lo, hi);
            MoveToEx(dc, area.left, y, nullptr);
            LineTo(dc, area.right, y);
            RECT r = { Px(4), y - Px(8), area.left - Px(6), y + Px(8) };
            DrawTextW(dc, label.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        RECT first = { area.left - Px(20), area.bottom + Px(2), area.left + Px(60), h };
        DrawTextW(dc, L"1", -1, &first, DT_LEFT | DT_SINGLELINE);
        RECT last = { area.right - Px(320), area.bottom + Px(2), area.right + Px(4), h };
        DrawTextW(dc, (std::to_wstring(n) + L" (files, in the order of the tree)").c_str(), -1, &last, DT_RIGHT | DT_SINGLELINE);
        // The image shown, and the file under the mouse
        auto vertical = [&](int i, COLORREF color) {
            if (i < 0 || i >= n) return;
            HPEN pen = CreatePen(PS_SOLID, 1, color);
            SelectObject(dc, pen);
            MoveToEx(dc, X(i), area.top, nullptr);
            LineTo(dc, X(i), area.bottom);
            SelectObject(dc, axis);
            DeleteObject(pen);
        };
        vertical(now, RGB(230, 120, 0));
        vertical(hover, RGB(150, 150, 150));
        // The values: a line for each number (broken where a file has none), dots when they are few
        static const COLORREF colors[3] = { RGB(0, 90, 230), RGB(220, 60, 60), RGB(0, 150, 80) };
        const size_t lines = numeric ? (std::max)(parts, static_cast<size_t>(1)) : 1;
        for (size_t k = 0; k < lines; k++)
        {
            HPEN pen = CreatePen(PS_SOLID, Px(1) + (n < 200 ? 1 : 0), colors[k % 3]);
            HBRUSH dot = CreateSolidBrush(colors[k % 3]);
            SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, dot);
            std::vector<POINT> run;
            auto flush = [&]() { if (run.size() > 1) Polyline(dc, run.data(), static_cast<int>(run.size())); run.clear(); };
            for (int i = 0; i < n; i++)
            {
                double v;
                if (numeric) { if (k >= numbers[i].size()) { flush(); continue; } v = numbers[i][k]; }
                else { if (category[i] < 0) { flush(); continue; } v = category[i]; }
                const POINT p = { X(i), Y(v, lo, hi) };
                run.push_back(p);
                if (n <= 300) Rectangle(dc, p.x - Px(2), p.y - Px(2), p.x + Px(2) + 1, p.y + Px(2) + 1);
            }
            flush();
            SelectObject(dc, oldBrush);
            SelectObject(dc, axis);
            DeleteObject(pen);
            DeleteObject(dot);
        }
        SelectObject(dc, oldPen);
        DeleteObject(axis);
    }
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldCanvas);
    DeleteObject(canvas);
    DeleteDC(dc);
}

LRESULT CALLBACK Plot::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<Plot*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        self = static_cast<Plot*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
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
    case WM_MOUSEMOVE:
        if (self)
        {
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int i = pt.y >= self->area.top - self->Px(4) && pt.y <= self->area.bottom + self->Px(4) ? self->At(pt.x) : -1;
            if (i != self->hover)
            {
                self->hover = i;
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (self && self->hover >= 0) { self->hover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        if (self)
        {
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&self->closeBox, pt)) { self->Show(false); if (self->closed) self->closed(); return 0; }
            if (self->hover >= 0 && self->picked) self->picked(self->hover);
        }
        return 0;
    case WM_SETCURSOR:
        if (self && LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursor(nullptr, PtInRect(&self->closeBox, pt) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_NCDESTROY:
        if (self) self->wnd = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
