// The colors of the studies, the icons of DICOM files (their series number on the color of their study), and the handlers chained before us
#include "Shell.h"

// ---------------------------------------------------------------------------------------------
// Colors: the palette and hash of FolderIcons in explorer4dicoms.ps1

int ColorIndex(const std::string& uid)
{
    unsigned long long h = 14695981039346656037ULL;     // 64-bit FNV-1a, as ColorIndex in C#
    for (unsigned char c : uid) { h ^= c; h *= 1099511628211ULL; }
    return static_cast<int>((h >> 32) % PaletteSize);
}

static Rgb FromHex(unsigned v) { return { ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0 }; }
static COLORREF ToGdi(Rgb c)
{
    auto k = [](double x) { return static_cast<int>(x < 0 ? 0 : x > 1 ? 255 : x * 255 + 0.5); };
    return RGB(k(c.r), k(c.g), k(c.b));
}
static Rgb Mix(Rgb a, Rgb b, double t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }

// APCA (Accessible Perceptual Contrast Algorithm 0.0.98G, the contrast of the coming WCAG 3):
// perceptual lightness contrast that knows polarity, i.e. that dark text on a light background
// reads better than the same numbers reversed. |Lc| 60 and up is good for small text.
static double ApcaY(Rgb c)
{
    double y = 0.2126729 * pow(c.r, 2.4) + 0.7151522 * pow(c.g, 2.4) + 0.0721750 * pow(c.b, 2.4);
    return y < 0.022 ? y + pow(0.022 - y, 1.414) : y;
}
static double Apca(Rgb text, Rgb bg)
{
    double yt = ApcaY(text), yb = ApcaY(bg);
    if (yb > yt)
    {
        double s = (pow(yb, 0.56) - pow(yt, 0.57)) * 1.14;          // dark text on light
        return s < 0.1 ? 0 : (s - 0.027) * 100;
    }
    double s = (pow(yb, 0.65) - pow(yt, 0.62)) * 1.14;              // light text on dark
    return s > -0.1 ? 0 : (s + 0.027) * 100;
}

static Rgb FromHsl(double h, double s, double l)
{
    double c = (1 - fabs(2 * l - 1)) * s, hp = fmod(h, 360) / 60, x = c * (1 - fabs(fmod(hp, 2) - 1)), m = l - c / 2;
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    return { r + m, g + m, b + m };
}

static void ToHueSat(Rgb c, double& h, double& s)
{
    double mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    double mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    double d = mx - mn, l = (mx + mn) / 2;
    s = d == 0 ? 0 : d / (1 - fabs(2 * l - 1));
    if (d == 0) h = 0;
    else if (mx == c.r) h = 60 * fmod((c.g - c.b) / d + 6, 6);
    else if (mx == c.g) h = 60 * ((c.b - c.r) / d + 2);
    else h = 60 * ((c.r - c.g) / d + 4);
}

// The hue of the complementary color, as a pale tint or a deep shade: whichever gives the number
// more perceptual contrast. The plain complement can be as light as the text (red and its teal), and
// a dark background turns red, blue or purple digits unreadable at 16 px; APCA picks the pale tint
// for them and keeps the deep shade for the light colors (lime, mint, pink, white...).
Rgb Background(Rgb text)
{
    double h, s;
    ToHueSat(text, h, s);
    double tint = s < 0.1 ? 0 : 0.55;                  // grays: neutral background
    Rgb pale = FromHsl(h + 180, tint, 0.92);
    Rgb deep = FromHsl(h + 180, tint, 0.14);
    return fabs(Apca(text, pale)) >= fabs(Apca(text, deep)) ? pale : deep;
}

static double Luminance(Rgb c) { return ApcaY(c); }

// ---------------------------------------------------------------------------------------------
// Drawing

// Up to 24 px without antialiasing: smoothing blends the 1 px strokes of the digits with the
// background and washes their color out; hinted, unsmoothed digits keep the exact study color
static HFONT MakeFont(int height, int width, const wchar_t* face, int weight, bool smooth = true)
{
    return CreateFontW(-height, width, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, smooth ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY,
                       VARIABLE_PITCH | FF_SWISS, face);
}

// GDI silently replaces a font it does not have: the face really selected tells
static bool HasFace(HDC dc, const wchar_t* face)
{
    HFONT f = MakeFont(12, 0, face, FW_NORMAL);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t got[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, old);
    DeleteObject(f);
    return _wcsnicmp(got, face, LF_FACESIZE - 1) == 0;
}

struct Glyph
{
    int width;          // advance width of the whole number
    int avgChar;        // average character width of the font
    int ascent;         // font ascent (baseline below the top of the text cell)
    int digitTop;       // ink of the digits: top above the baseline, and height
    int digitHeight;
};

// Measures the number, and the ink of the digit 8 (as tall as any digit)
static Glyph Measure(HDC dc, HFONT font, const std::wstring& label)
{
    Glyph g = {};
    HGDIOBJ old = SelectObject(dc, font);
    SIZE ext = {};
    GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &ext);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    GLYPHMETRICS gm = {};
    MAT2 identity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    if (GetGlyphOutlineW(dc, L'8', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR)
    {
        g.digitTop = gm.gmptGlyphOrigin.y;
        g.digitHeight = static_cast<int>(gm.gmBlackBoxY);
    }
    else
    {
        g.digitTop = tm.tmAscent - tm.tmInternalLeading;
        g.digitHeight = g.digitTop;
    }
    SelectObject(dc, old);
    g.width = ext.cx;
    g.avgChar = tm.tmAveCharWidth;
    g.ascent = tm.tmAscent;
    return g;
}

// The square of a series icon (the number in the study color) drawn at (x0, y0): the whole icon, or
// the badge in the corner of a thumbnail
void DrawBadge(HDC dc, int x0, int y0, int size, int color, const std::wstring& label)
{
    Rgb text = FromHex(Palette[color]);
    Rgb bg = Background(text);
    RECT all = { x0, y0, x0 + size, y0 + size };
    HBRUSH bgBrush = CreateSolidBrush(ToGdi(bg));
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);
    // A thin border in a darker (or lighter) tone of the background, so it never melts into the view
    Rgb edge = Mix(bg, Luminance(bg) > 0.18 ? Rgb{ 0, 0, 0 } : Rgb{ 1, 1, 1 }, 0.35);
    HBRUSH edgeBrush = CreateSolidBrush(ToGdi(edge));
    FrameRect(dc, &all, edgeBrush);
    DeleteObject(edgeBrush);

    // The largest font whose number fits inside the border: Bahnschrift (the signage typeface of
    // Windows, digits made to be read small), its condensed cut for 3 digits or more; Segoe UI where
    // there is no Bahnschrift. Squeezed a little before getting smaller.
    const wchar_t* face = label.size() >= 3 ? L"Bahnschrift SemiBold Condensed" : L"Bahnschrift SemiBold";
    int weight = FW_NORMAL;                             // the weight is in the face name
    if (!HasFace(dc, face)) { face = L"Segoe UI"; weight = FW_BOLD; }
    bool smooth = size > 24;
    int margin = size >= 32 ? size / 16 : 1;
    int maxWidth = size - 2 - 2 * (margin - 1);         // inside the 1 px border
    int maxHeight = size - 2 - 2 * margin;
    HFONT font = nullptr;
    Glyph g = {};
    for (int height = size * 3 / 2; height >= 6 && !font; height--)
    {
        for (int squeeze = 0; squeeze <= 1 && !font; squeeze++)
        {
            int width = 0;
            if (squeeze)
            {
                // Narrower glyphs at the same height, down to 75 % of their width
                HFONT probe = MakeFont(height, 0, face, weight, smooth);
                Glyph p = Measure(dc, probe, label);
                DeleteObject(probe);
                if (p.width <= maxWidth || p.avgChar == 0) continue;
                width = p.avgChar * maxWidth / p.width;
                if (width < p.avgChar * 3 / 4) continue;
            }
            HFONT f = MakeFont(height, width, face, weight, smooth);
            Glyph m = Measure(dc, f, label);
            if (m.width <= maxWidth && m.digitHeight <= maxHeight) { font = f; g = m; }
            else DeleteObject(f);
        }
    }
    if (font)
    {
        HGDIOBJ old = SelectObject(dc, font);
        // Centered on the ink of the digits: baseline - digitTop is where their tops are
        int x = x0 + (size - g.width) / 2;
        int y = y0 + (size - g.digitHeight + 1) / 2 + g.digitTop - g.ascent;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToGdi(text));
        TextOutW(dc, x, y, label.c_str(), static_cast<int>(label.size()));
        SelectObject(dc, old);
        DeleteObject(font);
    }
}

HICON DrawIcon(int size, int color, const std::wstring& label)
{
    if (size < 8 || size > 512) size = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;                      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(dc); return nullptr; }
    HGDIOBJ oldBitmap = SelectObject(dc, dib);
    DrawBadge(dc, 0, 0, size, color, label);
    GdiFlush();

    // GDI leaves the alpha channel at 0: the whole square is opaque, but for the four corners
    auto px = static_cast<DWORD*>(bits);
    for (int i = 0; i < size * size; i++) px[i] |= 0xFF000000;
    if (size >= 16) px[0] = px[size - 1] = px[size * (size - 1)] = px[size * size - 1] = 0;

    SelectObject(dc, oldBitmap);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = dib;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(dib);
    DeleteDC(dc);
    return icon;
}

// ---------------------------------------------------------------------------------------------
// COM object: IPersistFile receives the file, IExtractIconW gives its icon

// ---------------------------------------------------------------------------------------------
// Chaining: the handlers another program had for an extension (files without one included) before the
// install put these in front of them (tools\Register-DicomIcons.ps1 in HKCU\Software\explorer4dicoms\
// Chain\<ext>, Register-DicomProperties.ps1 in HKLM\SOFTWARE\DicomStudyCount\PropertyHandler\Chain\<ext>).
// Files that are not DICOM go to them, as if this DLL were not there.

// ".ima" (lowercase), or "(none)"
static std::wstring ChainName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/:"), dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) || dot + 1 == path.size()) return L"(none)";
    std::wstring ext = path.substr(dot);
    CharLowerBuffW(&ext[0], static_cast<DWORD>(ext.size()));
    return ext;
}

// What was there before for the extension of `path`: a CLSID, or the location of an icon
std::wstring Previous(HKEY root, const wchar_t* base, const std::wstring& path, const wchar_t* name)
{
    const std::wstring key = std::wstring(base) + L"\\" + ChainName(path);
    wchar_t value[1024];
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return std::wstring();
    return value;
}

// The first bytes of a file: "DICM" after the preamble, or a "raw" DICOM (group 0002 or 0008 first),
// as ReadHeader tries them
bool LooksDicom(const unsigned char* b, size_t n)
{
    if (n >= 132 && b[128] == 'D' && b[129] == 'I' && b[130] == 'C' && b[131] == 'M') return true;
    return n >= 8 && b[1] == 0 && (b[0] == 0x02 || b[0] == 0x08);
}

bool StreamLooksDicom(IStream* stream)
{
    unsigned char head[132] = {};
    LARGE_INTEGER zero = {};
    ULONG got = 0, n = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    while (got < sizeof(head) && SUCCEEDED(stream->Read(head + got, sizeof(head) - got, &n)) && n > 0) got += n;
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    return LooksDicom(head, got);
}

// The name of the file of a stream (for its extension), empty if it has none
std::wstring StreamName(IStream* stream)
{
    STATSTG stat = {};
    std::wstring name;
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_DEFAULT)) && stat.pwcsName)
    {
        name = stat.pwcsName;
        CoTaskMemFree(stat.pwcsName);
    }
    return name;
}

IconHandler::IconHandler() { InterlockedIncrement(&g_locks); }

IconHandler::~IconHandler()
{
    if (previous) previous->Release();
    InterlockedDecrement(&g_locks);
}

// IUnknown
IFACEMETHODIMP IconHandler::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IExtractIconW) *ppv = static_cast<IExtractIconW*>(this);
    else if (riid == IID_IPersist || riid == IID_IPersistFile) *ppv = static_cast<IPersistFile*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) IconHandler::AddRef() { return InterlockedIncrement(&refs); }

IFACEMETHODIMP_(ULONG) IconHandler::Release()
{
    LONG r = InterlockedDecrement(&refs);
    if (r == 0) delete this;
    return r;
}

// IPersistFile
IFACEMETHODIMP IconHandler::GetClassID(CLSID* clsid) { if (!clsid) return E_POINTER; *clsid = CLSID_DicomIcons; return S_OK; }

IFACEMETHODIMP IconHandler::IsDirty() { return S_FALSE; }

IFACEMETHODIMP IconHandler::Load(LPCOLESTR file, DWORD)
{
    if (!file) return E_INVALIDARG;
    try { path = file; } catch (...) { return E_OUTOFMEMORY; }
    return S_OK;
}

IFACEMETHODIMP IconHandler::Save(LPCOLESTR, BOOL) { return E_NOTIMPL; }

IFACEMETHODIMP IconHandler::SaveCompleted(LPCOLESTR) { return E_NOTIMPL; }

IFACEMETHODIMP IconHandler::GetCurFile(LPOLESTR*) { return E_NOTIMPL; }

// IExtractIconW: the location is a key ("DicomStudyCount|v2|<color>|<number>") that Explorer
// caches the drawn icon under, so every color and number is drawn only once
IFACEMETHODIMP IconHandler::GetIconLocation(UINT flags, LPWSTR file, UINT cch, int* index, UINT* outFlags)
{
    if (!file || !index || !outFlags || cch == 0) return E_INVALIDARG;
    // Reading the file can be slow (network): Explorer calls again on a background thread
    if (flags & GIL_ASYNC) return E_PENDING;
    try
    {
        Header h;
        if (path.empty() || ReadHeader(path.c_str(), h, UpToSeriesNumber) != Kind::Dicom)
        {
            // Not DICOM: the icon it had before the install (its handler, or its fixed icon)
            if (!path.empty())
            {
                if (!previous && (previous = CreatePrevious<IExtractIconW>(Previous(HKEY_CURRENT_USER, ChainKey, path, L"IconHandler"))))
                {
                    IPersistFile* load = nullptr;
                    HRESULT hr = previous->QueryInterface(IID_PPV_ARGS(&load));
                    if (SUCCEEDED(hr)) { hr = load->Load(path.c_str(), STGM_READ); load->Release(); }
                    if (FAILED(hr)) { previous->Release(); previous = nullptr; }
                }
                if (previous) return previous->GetIconLocation(flags, file, cch, index, outFlags);
                if (FixedIcon(Previous(HKEY_CURRENT_USER, ChainKey, path, L"Icon"), file, cch, index))
                {
                    *outFlags = 0;
                    return S_OK;
                }
            }
            // Nothing before: the generic file icon
            wchar_t sys[MAX_PATH];
            UINT n = GetSystemDirectoryW(sys, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return S_FALSE;
            std::wstring generic = std::wstring(sys) + L"\\shell32.dll";
            if (generic.size() + 1 > cch) return S_FALSE;
            wcscpy_s(file, cch, generic.c_str());
            *index = 0;
            *outFlags = 0;
            return S_OK;
        }
        int color = h.studyUid.empty() ? NoStudyColor : ColorIndex(h.studyUid);
        std::wstring label;
        for (char c : h.seriesNumber) if (label.size() < 6) label += static_cast<wchar_t>(static_cast<unsigned char>(c));
        if (label.empty()) label = L"?";
        std::wstring key = std::wstring(KeyPrefix) + std::to_wstring(color) + L"|" + label;
        if (key.size() + 1 > cch) return S_FALSE;
        wcscpy_s(file, cch, key.c_str());
        *index = 0;
        *outFlags = GIL_NOTFILENAME | GIL_PERINSTANCE;
        return S_OK;
    }
    catch (...) { return S_FALSE; }
}

IFACEMETHODIMP IconHandler::Extract(LPCWSTR file, UINT index, HICON* largeIcon, HICON* smallIcon, UINT sizes)
{
    if (!file) return E_INVALIDARG;
    try
    {
        std::wstring key = file;
        size_t prefix = wcslen(KeyPrefix);
        if (key.compare(0, prefix, KeyPrefix) != 0)
        {
            // The icon of the handler from before; another one (generic, fixed): Explorer extracts it
            if (previous) return previous->Extract(file, index, largeIcon, smallIcon, sizes);
            return S_FALSE;
        }
        size_t bar = key.find(L'|', prefix);
        if (bar == std::wstring::npos) return S_FALSE;
        int color = _wtoi(key.substr(prefix, bar - prefix).c_str());
        if (color < 0 || color >= PaletteSize) return S_FALSE;
        std::wstring label = key.substr(bar + 1);
        if (largeIcon) *largeIcon = DrawIcon(LOWORD(sizes), color, label);
        if (smallIcon) *smallIcon = DrawIcon(HIWORD(sizes), color, label);
        return S_OK;
    }
    catch (...) { return S_FALSE; }
}

// A fixed icon from before ("C:\...\imageres.dll,-5205"), not "%1" (each file its own icon)
bool IconHandler::FixedIcon(const std::wstring& location, LPWSTR file, UINT cch, int* index)
{
    if (location.empty() || location.find(L"%1") != std::wstring::npos) return false;
    std::wstring where = location;
    int number = 0;
    const size_t comma = where.find_last_of(L',');
    if (comma != std::wstring::npos)
    {
        number = _wtoi(where.c_str() + comma + 1);
        where.resize(comma);
    }
    if (where.size() >= 2 && where.front() == L'"' && where.back() == L'"') where = where.substr(1, where.size() - 2);
    wchar_t expanded[MAX_PATH];
    const DWORD n = ExpandEnvironmentStringsW(where.c_str(), expanded, MAX_PATH);
    if (n == 0 || n > MAX_PATH || wcslen(expanded) + 1 > cch) return false;
    wcscpy_s(file, cch, expanded);
    *index = number;
    return true;
}
