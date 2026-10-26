// The preview of Explorer's preview pane: the image, its header, the folder's series, the DICOMDIR (its
// window and its interfaces; the other parts in Preview*.cpp)
#include "Shell.h"

// The common controls 6 (themed check boxes and headers) for a DLL without a manifest of its own:
// the one of shell32.dll (resource 124), as an activation context
HANDLE ThemedControls()
{
    static HANDLE context = [] {
        wchar_t shell32[MAX_PATH];
        UINT n = GetSystemDirectoryW(shell32, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 12) return INVALID_HANDLE_VALUE;
        wcscat_s(shell32, L"\\shell32.dll");
        ACTCTXW a = { sizeof(a) };
        a.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
        a.lpSource = shell32;
        a.lpResourceName = MAKEINTRESOURCEW(124);
        return CreateActCtxW(&a);
    }();
    return context;
}

// The series of a file, for the preview to step through it: the files of its folder with its
// SeriesInstanceUID, in the order of their slices (the Z level, as the column), then of their
// InstanceNumber and names. The headers of a folder are read once and kept for the next previews (of
// the last few folders) while the folder does not change (its last write time).
class FolderHeaders
{
public:
    // Empty if it cannot be known, or if `cancel` is set while the folder is read
    std::vector<std::wstring> SeriesOf(const std::wstring& path, const std::wstring& folder, volatile LONG* cancel)
    {
        std::vector<std::wstring> files;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (folder.empty() || !GetFileAttributesExW(folder.c_str(), GetFileExInfoStandard, &a)) return files;
        const std::wstring key = Lower(folder);
        std::vector<Entry> entries;
        bool known = false;
        AcquireSRWLockShared(&lock);
        auto it = folders.find(key);
        if (it != folders.end() && CompareFileTime(&it->second.written, &a.ftLastWriteTime) == 0)
        {
            entries = it->second.entries;
            known = true;
        }
        ReleaseSRWLockShared(&lock);
        if (!known)
        {
            if (!Read(folder, entries, cancel)) return files;
            AcquireSRWLockExclusive(&lock);
            if (folders.size() >= 8) folders.clear();
            folders[key] = { a.ftLastWriteTime, entries };
            ReleaseSRWLockExclusive(&lock);
        }
        const std::wstring self = Lower(path);
        auto me = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return Lower(e.path) == self; });
        if (me == entries.end()) return files;
        const std::string uid = me->seriesUid;
        std::vector<Entry> series;
        for (auto& e : entries) if (e.seriesUid == uid) series.push_back(e);
        const bool allZ = std::all_of(series.begin(), series.end(), [](const Entry& e) { return e.hasZ; });
        std::sort(series.begin(), series.end(), [&](const Entry& x, const Entry& y) {
            if (allZ && x.z != y.z) return x.z < y.z;
            if (x.instance != y.instance) return x.instance < y.instance;
            return CompareStringOrdinal(x.path.c_str(), -1, y.path.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        for (auto& e : series) files.push_back(e.path);
        return files;
    }

private:
    struct Entry { std::wstring path; std::string seriesUid; bool hasZ = false; double z = 0; long instance = 0; };
    struct Folder { FILETIME written; std::vector<Entry> entries; };
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, Folder> folders;          // lowercase path

    static std::wstring Lower(std::wstring s)
    {
        if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
        return s;
    }

    // The header of every file, up to ImageOrientationPatient
    static bool Read(const std::wstring& folder, std::vector<Entry>& entries, volatile LONG* cancel)
    {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW((folder + (folder.back() == L'\\' ? L"*" : L"\\*")).c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return false;
        bool done = true;
        do
        {
            if (*cancel) { done = false; break; }
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) continue;
            Entry e;
            e.path = folder + (folder.back() == L'\\' ? L"" : L"\\") + fd.cFileName;
            Header h;
            if (ReadHeader(e.path.c_str(), h, 0x00200037) != Kind::Dicom || h.seriesUid.empty()) continue;
            e.seriesUid = h.seriesUid;
            e.hasZ = ZLevel(h, e.z);
            e.instance = atol(h.instanceNumber.c_str());
            entries.push_back(std::move(e));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        return done;
    }
};

static FolderHeaders g_folderHeaders;

static DWORD WINAPI ScanSeries(void* param)
{
    auto scan = static_cast<std::shared_ptr<SeriesScan>*>(param);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low disk priority too
    try
    {
        (*scan)->files = g_folderHeaders.SeriesOf((*scan)->path, (*scan)->folder, &(*scan)->cancel);
        if (!(*scan)->cancel) PostMessageW((*scan)->window, (*scan)->message, 0, 0);
        if (!(*scan)->cancel && (*scan)->segmentsMessage && !(*scan)->study.empty())
        {
            (*scan)->segmentations = SegmentationsNear((*scan)->path, (*scan)->study);
            if (!(*scan)->cancel && !(*scan)->segmentations.empty()) PostMessageW((*scan)->window, (*scan)->segmentsMessage, 0, 0);
        }
    }
    catch (...) {}
    delete scan;
    FreeLibraryAndExitThread(g_module, 0);         // the reference StartSeriesScan took
}

static std::shared_ptr<SeriesScan> StartSeriesScan(const std::wstring& path, const std::wstring& folder, const std::string& study, HWND window,
                                                     UINT message, UINT segmentsMessage)
{
    auto scan = std::make_shared<SeriesScan>();
    scan->path = path;
    scan->folder = folder;
    scan->window = window;
    scan->message = message;
    scan->study = study;
    scan->segmentsMessage = segmentsMessage;
    // The DLL stays loaded until the thread ends
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ScanSeries), &self)) return nullptr;
    auto param = new (std::nothrow) std::shared_ptr<SeriesScan>(scan);
    HANDLE thread = param ? CreateThread(nullptr, 0, ScanSeries, param, 0, nullptr) : nullptr;
    if (!thread)
    {
        delete param;
        FreeLibrary(self);
        return nullptr;
    }
    CloseHandle(thread);
    return scan;
}

PreviewHandler::PreviewHandler() { InterlockedIncrement(&g_locks); }

PreviewHandler::~PreviewHandler()
{
    Unload();
    if (site) site->Release();
    InterlockedDecrement(&g_locks);
}

// IUnknown
IFACEMETHODIMP PreviewHandler::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IPreviewHandler) *ppv = static_cast<IPreviewHandler*>(this);
    else if (riid == IID_IInitializeWithItem) *ppv = static_cast<IInitializeWithItem*>(this);
    else if (riid == IID_IPreviewHandlerVisuals) *ppv = static_cast<IPreviewHandlerVisuals*>(this);
    else if (riid == IID_IOleWindow) *ppv = static_cast<IOleWindow*>(this);
    else if (riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) PreviewHandler::AddRef() { return InterlockedIncrement(&refs); }

IFACEMETHODIMP_(ULONG) PreviewHandler::Release()
{
    LONG r = InterlockedDecrement(&refs);
    if (r == 0) delete this;
    return r;
}

// IInitializeWithItem (not a stream): the path is needed, for the folder (its tab, its Explorer
// window) and for the values of the columns. The host asks for a stream first, so there is none.
IFACEMETHODIMP PreviewHandler::Initialize(IShellItem* i, DWORD)
{
    if (!i) return E_INVALIDARG;
    if (item) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    item = i;
    item->AddRef();
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
    {
        path = p;
        CoTaskMemFree(p);
        size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) folder = path.substr(0, slash == 2 && path[1] == L':' ? 3 : slash);
    }
    return S_OK;
}

// IPreviewHandler
IFACEMETHODIMP PreviewHandler::SetWindow(HWND hwnd, const RECT* rc)
{
    if (!hwnd || !rc) return E_INVALIDARG;
    parent = hwnd;
    bounds = *rc;
    if (previous) return previous->SetWindow(hwnd, rc);
    if (wnd)
    {
        ::SetParent(wnd, parent);
        Place();
    }
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::SetRect(const RECT* rc)
{
    if (!rc) return E_INVALIDARG;
    bounds = *rc;
    if (previous) return previous->SetRect(rc);
    if (wnd) Place();
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::DoPreview()
{
    if (wnd || previous) return S_OK;
    if (!item || !parent) return E_UNEXPECTED;
    try
    {
        IStream* stream = nullptr;
        HRESULT hr = item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&stream));
        if (FAILED(hr)) return hr;
        // Not DICOM: the preview from before, if there was one (not even read here: it can be a
        // disk image of gigabytes)
        if (!StreamLooksDicom(stream))
        {
            const bool chained = StartPrevious(stream);
            stream->Release();
            if (chained) return S_OK;
            view = Rendered();
            view.summary = L"Not a DICOM file";
            plain = true;
            return CreateWindows();
        }
        std::vector<char> data;
        hr = ReadAll(stream, data);
        stream->Release();
        if (FAILED(hr)) return hr;
        view = RenderDicom(data);
    }
    catch (...) { view = Rendered(); view.summary = L"This file cannot be shown"; }
    return CreateWindows();
}

IFACEMETHODIMP PreviewHandler::Unload()
{
    if (previous)
    {
        previous->Unload();
        IObjectWithSite* s = nullptr;
        if (SUCCEEDED(previous->QueryInterface(IID_PPV_ARGS(&s)))) { s->SetSite(nullptr); s->Release(); }
        previous->Release();
        previous = nullptr;
    }
    if (scan) { InterlockedExchange(&scan->cancel, 1); scan.reset(); }
    if (wnd) { DestroyWindow(wnd); wnd = nullptr; edit = nullptr; list = nullptr; tree = nullptr; }
    directory.clear();
    groups.clear();
    group = -1;
    series.clear();
    slice = -1;
    nearby.clear();
    SetSegmentation(0, 0, {}, {});
    current.clear();
    playing = jumping = stale = windowSet = plain = false;
    for (HFONT* f : { &font, &uiFont, &boldFont }) if (*f) { DeleteObject(*f); *f = nullptr; }
    if (background) { DeleteObject(background); background = nullptr; }
    if (item) { item->Release(); item = nullptr; }
    path.clear();
    folder.clear();
    note.clear();
    view = Rendered();
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::SetFocus()
{
    if (previous) return previous->SetFocus();
    HWND content = Content();
    if (!content) return S_FALSE;
    ::SetFocus(content);
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::QueryFocus(HWND* focus)
{
    if (!focus) return E_INVALIDARG;
    if (previous) return previous->QueryFocus(focus);
    *focus = GetFocus();
    return *focus ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

IFACEMETHODIMP PreviewHandler::TranslateAccelerator(MSG* msg)
{
    if (!msg) return E_INVALIDARG;
    if (previous) return previous->TranslateAccelerator(msg);
    // In the header: Ctrl+A selects it all, Ctrl+C (or Ctrl+Insert) copies the selection; in the
    // columns the same with rows. Ctrl+T: between the two headers. Explorer would take Ctrl+C to
    // copy the file.
    HWND focus = GetFocus();
    if (msg->message == WM_KEYDOWN && focus && focus == wnd && Key(msg->wParam)) return S_OK;
    if (msg->message == WM_KEYDOWN && focus && (focus == edit || focus == list) && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        switch (msg->wParam)
        {
        case 'A':
            if (focus == edit) SendMessageW(edit, EM_SETSEL, 0, -1);
            else ListView_SetItemState(list, -1, LVIS_SELECTED, LVIS_SELECTED);
            return S_OK;
        case 'C': case VK_INSERT:
            if (focus == edit) SendMessageW(edit, WM_COPY, 0, 0);
            else CopyRows();
            return S_OK;
        case 'T':
            SelectTab(tab == TabHeader ? TabTags : TabHeader);
            return S_OK;
        }
    }
    // Keys this window does not use go back to Explorer (the frame)
    IPreviewHandlerFrame* frame = nullptr;
    HRESULT hr = S_FALSE;
    if (site && SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&frame))))
    {
        hr = frame->TranslateAccelerator(msg);
        frame->Release();
    }
    return hr;
}

// IPreviewHandlerVisuals: the colors of Explorer's theme (dark mode included)
IFACEMETHODIMP PreviewHandler::SetBackgroundColor(COLORREF color)
{
    backColor = color;
    if (background) { DeleteObject(background); background = nullptr; }
    Recolor();
    if (IPreviewHandlerVisuals* v = PreviousVisuals()) { v->SetBackgroundColor(color); v->Release(); }
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::SetFont(const LOGFONTW* f)
{
    if (f) { hostFont = *f; hasFont = true; }
    if (IPreviewHandlerVisuals* v = PreviousVisuals()) { v->SetFont(f); v->Release(); }
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::SetTextColor(COLORREF color)
{
    textColor = color;
    Recolor();
    if (IPreviewHandlerVisuals* v = PreviousVisuals()) { v->SetTextColor(color); v->Release(); }
    return S_OK;
}

// IOleWindow
IFACEMETHODIMP PreviewHandler::GetWindow(HWND* hwnd)
{
    if (!hwnd) return E_INVALIDARG;
    *hwnd = parent;
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::ContextSensitiveHelp(BOOL) { return E_NOTIMPL; }

// IObjectWithSite
IFACEMETHODIMP PreviewHandler::SetSite(IUnknown* s)
{
    if (site) site->Release();
    site = s;
    if (site) site->AddRef();
    IObjectWithSite* p = nullptr;
    if (previous && SUCCEEDED(previous->QueryInterface(IID_PPV_ARGS(&p)))) { p->SetSite(s); p->Release(); }
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::GetSite(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    return site ? site->QueryInterface(riid, ppv) : E_FAIL;
}

// The DICOM explorer hosts it in its window: only the image (no tabs), of the files it gives, and
// tells it which one is shown when the slider or the cine moves (`moved`)
HRESULT PreviewHandler::Host(HWND host, const RECT& rc)
{
    hosted = true;
    parent = host;
    bounds = rc;
    return CreateWindows();
}

void PreviewHandler::ShowIndex(int index) { GoSlice(index); }    // another one of the same files

bool PreviewHandler::Stepping() const { return playing || jumping || slice >= 0; }     // (going through the files)

bool PreviewHandler::Running() const { return playing || jumping; }

void PreviewHandler::StartCine() { Play(true); }

bool PreviewHandler::Overlay() const { return overlay; }

// Hosted: lines over the image (reference lines: where other planes cut it), and a small crosshair, in
// the coordinates of its pixels (columns, rows; 0 the middle of the first one)
void PreviewHandler::SetGuides(std::vector<std::array<double, 4>> lines)
{
    guides = std::move(lines);
    if (wnd) InvalidateRect(wnd, &imageArea, FALSE);
}

void PreviewHandler::SetCrosshair(bool on, double column, double row)
{
    if (!on && !crosshair) return;
    crosshair = on;
    crossColumn = column;
    crossRow = row;
    if (wnd) InvalidateRect(wnd, &imageArea, FALSE);
}

void PreviewHandler::SetSegmentation(int width, int height, const std::vector<DWORD>& argb, std::vector<Outline> lines)
{
    if (maskBitmap) { DeleteObject(maskBitmap); maskBitmap = nullptr; }
    maskWidth = maskHeight = 0;
    if (width > 0 && height > 0 && argb.size() >= static_cast<size_t>(width) * height)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        maskBitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (maskBitmap && bits) { memcpy(bits, argb.data(), static_cast<size_t>(width) * height * 4); maskWidth = width; maskHeight = height; }
    }
    outlines = std::move(lines);
    if (wnd) InvalidateRect(wnd, &imageArea, FALSE);
}

void PreviewHandler::SetOverlay(bool on)
{
    overlay = on;
    DWORD value = overlay ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, Settings, L"Overlay", REG_DWORD, &value, sizeof(value));
    if (wnd) { InvalidateRect(wnd, &imageArea, FALSE); InvalidateRect(wnd, &barArea, FALSE); }
}

void PreviewHandler::SetSegmentsShown(bool on)
{
    if (on == segments) return;
    segments = on;
    if (!hosted)
    {
        DWORD value = segments ? 1 : 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, Settings, L"Segmentations", REG_DWORD, &value, sizeof(value));
    }
    if (wnd) { InvalidateRect(wnd, &imageArea, FALSE); InvalidateRect(wnd, &barArea, FALSE); }
}

// The image shown, as drawn (0xRRGGBB, top-down): none if there is no image
const std::vector<DWORD>* PreviewHandler::Pixels(int& width, int& height) const
{
    if (view.pixels.empty()) return nullptr;
    width = view.width;
    height = view.height;
    return &view.pixels;
}

void PreviewHandler::ShowPoster(std::vector<PosterFrame> frames, int delay, const std::wstring& caption)
{
    if (!hosted || frames.empty()) return;
    Play(false);
    Jump(false);
    const bool had = !view.pixels.empty();
    poster = std::move(frames);
    posterFrame = 0;
    view = Rendered();
    view.summary = caption;
    PosterPixels();
    slice = -1;
    current.clear();
    zoom = 1;
    KillTimer(wnd, TimerPoster);
    if (poster.size() > 1) SetTimer(wnd, TimerPoster, static_cast<UINT>((std::max)(20, delay)), nullptr);
    SetWindowTextW(wnd, view.summary.c_str());
    Shown(had);
}

// The file shown and the window dragged over it (monochrome): the exports of the explorer take it
bool PreviewHandler::DraggedWindow(std::wstring& file, double& center, double& width) const
{
    if (!windowSet || !view.imaging || !view.imaging->image || !view.imaging->mono) return false;
    file = current;
    center = view.imaging->center;
    width = view.imaging->width;
    return true;
}

void PreviewHandler::PosterPixels()
{
    const PosterFrame& p = poster[posterFrame % poster.size()];
    view.width = p.width;
    view.height = p.height;
    view.pixels = p.px;
}

void PreviewHandler::StopPoster()
{
    if (poster.empty()) return;
    KillTimer(wnd, TimerPoster);
    poster.clear();
}

IPreviewHandlerVisuals* PreviewHandler::PreviousVisuals() const
{
    IPreviewHandlerVisuals* v = nullptr;
    if (previous) previous->QueryInterface(IID_PPV_ARGS(&v));
    return v;
}

// The preview from before for this extension, in this window: initialized as it can be (the item,
// the path or the stream), with the site and colors of this one
bool PreviewHandler::StartPrevious(IStream* stream)
{
    IPreviewHandler* p = CreatePrevious<IPreviewHandler>(Previous(HKEY_CURRENT_USER, ChainKey, path, L"Preview"));
    if (!p) return false;
    HRESULT hr = E_NOINTERFACE;
    IInitializeWithItem* withItem = nullptr;
    IInitializeWithFile* withFile = nullptr;
    IInitializeWithStream* withStream = nullptr;
    if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&withItem)))) { hr = withItem->Initialize(item, STGM_READ); withItem->Release(); }
    else if (!path.empty() && SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&withFile)))) { hr = withFile->Initialize(path.c_str(), STGM_READ); withFile->Release(); }
    else if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&withStream)))) { hr = withStream->Initialize(stream, STGM_READ); withStream->Release(); }
    if (SUCCEEDED(hr))
    {
        IObjectWithSite* s = nullptr;
        if (site && SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&s)))) { s->SetSite(site); s->Release(); }
        IPreviewHandlerVisuals* v = nullptr;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&v))))
        {
            v->SetBackgroundColor(backColor);
            v->SetTextColor(textColor);
            if (hasFont) v->SetFont(&hostFont);
            v->Release();
        }
        hr = p->SetWindow(parent, &bounds);
        if (SUCCEEDED(hr)) hr = p->DoPreview();
    }
    if (FAILED(hr))
    {
        p->Unload();
        p->Release();
        return false;
    }
    previous = p;
    return true;
}

HBRUSH PreviewHandler::Background()
{
    if (!background) background = CreateSolidBrush(backColor);
    return background;
}

// Between the background and the text: lines and the inactive tabs
COLORREF PreviewHandler::Dim(int percent) const
{
    auto mix = [&](int shift) {
        return ((((textColor >> shift) & 0xFF) * percent + ((backColor >> shift) & 0xFF) * (100 - percent)) / 100) << shift;
    };
    return static_cast<COLORREF>(mix(0) | mix(8) | mix(16));
}

int PreviewHandler::Px(int n) const
{
    UINT dpi = wnd ? GetDpiForWindow(wnd) : 96;
    return MulDiv(n, dpi ? dpi : 96, 96);
}

HWND PreviewHandler::Content() const { return tab == TabColumns ? list : tab == TabDirectory ? tree : edit; }

DWORD PreviewHandler::ReadDword(const wchar_t* key, const wchar_t* name, DWORD fallback)
{
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

HRESULT PreviewHandler::CreateWindows()
{
    static ATOM cls = 0;
    if (!cls)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_module;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Explorer4DicomsPreview";
        cls = RegisterClassExW(&wc);
        if (!cls && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
    }
    // The tab of this folder (the columns at first), and the split of every preview
    if (!folder.empty() && !hosted)
        tab = static_cast<int>(ReadDword(L"Software\\explorer4dicoms\\Preview\\Tabs", folder.c_str(), TabColumns));
    if (tab < 0 || tab >= TabCount || tab == TabDirectory) tab = TabColumns;
    // A DICOMDIR opens on its tree
    directory = std::move(view.directory);
    if (!directory.empty()) tab = TabDirectory;
    split = static_cast<int>(ReadDword(Settings, L"Split", 450));
    overlay = ReadDword(Settings, L"Overlay", 1) != 0;
    if (!hosted) segments = ReadDword(Settings, L"Segmentations", 1) != 0;
    current = path;
    // Its (invisible) title is the summary line: for screen readers and tests
    wnd = CreateWindowExW(0, L"Explorer4DicomsPreview", view.summary.c_str(), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                          bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                          parent, nullptr, g_module, this);
    if (!wnd) return HRESULT_FROM_WIN32(GetLastError());
    const int points9 = -MulDiv(9, GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96, 72);
    font = CreateFontW(points9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    uiFont = CreateFontW(points9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    boldFont = CreateFontW(points9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    if (plain || hosted)
    {
        Place();
        return S_OK;
    }
    // The controls of the common controls 6 (themed), not the old ones prevhost.exe would give
    ULONG_PTR cookie = 0;
    HANDLE context = ThemedControls();
    bool active = context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    if (!directory.empty())
        tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                               TVS_FULLROWSELECT, 0, 0, 10, 10, wnd, nullptr, g_module, nullptr);
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                           ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                           0, 0, 10, 10, wnd, nullptr, g_module, nullptr);
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
                           0, 0, 10, 10, wnd, nullptr, g_module, nullptr);
    if (active) DeactivateActCtx(0, cookie);
    if (edit)
    {
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SendMessageW(edit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        SetWindowTextW(edit, (tab == TabTags ? view.tags : view.names).c_str());
    }
    if (list) FillColumns();
    if (tree) FillTree();
    else if (tab == TabDirectory) tab = TabColumns;
    KeepView();
    Recolor();
    Place();
    ShowContent();
    // Which columns Explorer shows: after DoPreview has returned, Explorer is not waiting for it
    if (list) PostMessageW(wnd, WM_READCOLUMNS, 0, 0);
    // The other files of its series, to step through them
    if (!view.pixels.empty() && !path.empty()) scan = StartSeriesScan(path, folder, view.studyUid, wnd, WM_SERIES, WM_SEGMENTS);
    return S_OK;
}
