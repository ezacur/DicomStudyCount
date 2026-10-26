// The preview of Explorer's preview pane: the image, its header, the folder's series, the DICOMDIR
#include "Shell.h"

// The DICOM columns, as the tab "Columns" of the preview lists them: the order of Properties > Details
static const PROPERTYKEY* const g_columnKeys[] = {
    &PKEY_StudyHash, &PKEY_SeriesHash, &PKEY_Modality, &PKEY_SeriesNumber, &PKEY_InstanceNumber, &PKEY_SeriesDescription,
    &PKEY_Protocol, &PKEY_Plane, &PKEY_SeriesKind, &PKEY_Acquired, &PKEY_TriggerTime, &PKEY_ZLevel, &PKEY_Rows, &PKEY_Columns,
    &PKEY_PixelSpacing, &PKEY_SliceThickness, &PKEY_Frames, &PKEY_ImageType, &PKEY_ObjectType, &PKEY_Compression,
    &PKEY_Manufacturer, &PKEY_FrameOfReference, &PKEY_StudyDate, &PKEY_PatientID, &PKEY_SensitiveData,
    &PKEY_ImageOrientation, &PKEY_StudyUid, &PKEY_SeriesUid, &PKEY_DuplicatedIn };

static std::wstring LongPath(const std::wstring& path)
{
    wchar_t buffer[32768];
    DWORD n = GetLongPathNameW(path.c_str(), buffer, ARRAYSIZE(buffer));
    return n > 0 && n < ARRAYSIZE(buffer) ? std::wstring(buffer, n) : path;
}

// The columns of the Explorer window (or tab) that shows a folder: the one the preview pane is in,
// or else any. Null if no Explorer window shows it. Explorer is another process: this needs the
// preview at medium integrity (DisableLowILProcessIsolation), a low one is denied ShellWindows.
static IColumnManager* ColumnsOf(const std::wstring& folder, HWND pane)
{
    if (folder.empty()) return nullptr;
    const std::wstring want = LongPath(folder);
    const HWND top = pane ? GetAncestor(pane, GA_ROOT) : nullptr;
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return nullptr;
    IColumnManager* found = nullptr;
    bool foundInPane = false;
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count && !foundInPane; i++)
    {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        IDispatch* d = nullptr;
        if (windows->Item(index, &d) != S_OK || !d) continue;
        SHANDLE_PTR hwnd = 0;
        IWebBrowserApp* app = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&app)))) { app->get_HWND(&hwnd); app->Release(); }
        IShellBrowser* browser = nullptr;
        IShellView* view = nullptr;
        if (SUCCEEDED(IUnknown_QueryService(d, SID_STopLevelBrowser, IID_PPV_ARGS(&browser))))
        {
            browser->QueryActiveShellView(&view);
            browser->Release();
        }
        d->Release();
        if (!view) continue;
        wchar_t path[MAX_PATH] = L"";
        IFolderView* fv = nullptr;
        IPersistFolder2* pf = nullptr;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv))) && SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf))) &&
            SUCCEEDED(pf->GetCurFolder(&pidl)))
            SHGetPathFromIDListW(pidl, path);
        if (pidl) CoTaskMemFree(pidl);
        if (pf) pf->Release();
        if (fv) fv->Release();
        if (path[0] && CompareStringOrdinal(LongPath(path).c_str(), -1, want.c_str(), -1, TRUE) == CSTR_EQUAL)
        {
            const bool inPane = top && reinterpret_cast<HWND>(hwnd) == top;
            IColumnManager* m = nullptr;
            if ((!found || inPane) && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&m))))
            {
                if (found) found->Release();
                found = m;
                foundInPane = inPane;
            }
        }
        view->Release();
    }
    windows->Release();
    return found;
}

static std::vector<PROPERTYKEY> VisibleColumns(IColumnManager* m)
{
    UINT n = 0;
    std::vector<PROPERTYKEY> keys;
    if (FAILED(m->GetColumnCount(CM_ENUM_VISIBLE, &n))) return keys;
    keys.resize(n);
    if (FAILED(m->GetColumns(CM_ENUM_VISIBLE, keys.data(), n))) keys.clear();
    return keys;
}

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
    }
    catch (...) {}
    delete scan;
    FreeLibraryAndExitThread(g_module, 0);         // the reference StartSeriesScan took
}

static std::shared_ptr<SeriesScan> StartSeriesScan(const std::wstring& path, const std::wstring& folder, HWND window, UINT message)
{
    auto scan = std::make_shared<SeriesScan>();
    scan->path = path;
    scan->folder = folder;
    scan->window = window;
    scan->message = message;
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
        wc.lpszClassName = L"DicomStudyCountPreview";
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
    current = path;
    // Its (invisible) title is the summary line: for screen readers and tests
    wnd = CreateWindowExW(0, L"DicomStudyCountPreview", view.summary.c_str(), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
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
    if (!view.pixels.empty() && !path.empty()) scan = StartSeriesScan(path, folder, wnd, WM_SERIES);
    return S_OK;
}

// The tree of a DICOMDIR: patients and studies open; the files of a series when it is opened
void PreviewHandler::FillTree()
{
    SendMessageW(tree, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), FALSE);
    TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    for (const DirNode& n : directory) AddNode(TVI_ROOT, n);
    if (HTREEITEM first = TreeView_GetRoot(tree)) TreeView_EnsureVisible(tree, first);
}

LPARAM PreviewHandler::Code(int g, int i) { return static_cast<LPARAM>((static_cast<long long>(g) << 32) | static_cast<unsigned>(i)); }

int PreviewHandler::GroupOf(LPARAM p) { return static_cast<int>(static_cast<long long>(p) >> 32); }

int PreviewHandler::FileOf(LPARAM p) { return static_cast<int>(static_cast<unsigned>(p & 0xFFFFFFFF)); }

// A record: those whose records are all files become a group (their tree items come when it is
// opened: a series can have thousands); a file alone is a group of its own
void PreviewHandler::AddNode(HTREEITEM under, const DirNode& n)
{
    const std::wstring base = folder + (!folder.empty() && folder.back() == L'\\' ? L"" : L"\\");
    TVINSERTSTRUCTW is = {};
    is.hParent = under;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    is.item.pszText = const_cast<wchar_t*>(n.text.c_str());
    is.item.lParam = -1;
    const bool files = !n.children.empty() &&
                       std::all_of(n.children.begin(), n.children.end(), [](const DirNode& c) { return c.children.empty() && !c.file.empty(); });
    if (files || (n.children.empty() && !n.file.empty()))
    {
        DirGroup g;
        if (files)
            for (const DirNode& c : n.children) { g.files.push_back(base + c.file); g.labels.push_back(c.text + L"   " + c.file); }
        else g.files.push_back(base + n.file);
        groups.push_back(std::move(g));
        is.item.lParam = Code(static_cast<int>(groups.size()) - 1, files ? -1 : 0);
        is.item.cChildren = files ? 1 : 0;
        std::wstring text = n.text + (files ? L"" : L"   " + n.file);
        is.item.pszText = const_cast<wchar_t*>(text.c_str());
        TreeView_InsertItem(tree, &is);
        return;
    }
    is.item.cChildren = n.children.empty() ? 0 : 1;
    HTREEITEM node = TreeView_InsertItem(tree, &is);
    for (const DirNode& c : n.children) AddNode(node, c);
    if (node && !n.children.empty()) TreeView_Expand(tree, node, TVE_EXPAND);
}

// A group opened: its files
void PreviewHandler::ExpandGroup(int g, HTREEITEM node)
{
    if (g < 0 || g >= static_cast<int>(groups.size()) || !groups[g].items.empty()) return;
    DirGroup& d = groups[g];
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < d.labels.size(); i++)
    {
        TVINSERTSTRUCTW is = {};
        is.hParent = node;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM;
        is.item.pszText = const_cast<wchar_t*>(d.labels[i].c_str());
        is.item.lParam = Code(g, static_cast<int>(i));
        d.items.push_back(TreeView_InsertItem(tree, &is));
    }
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
}

// An item chosen: the file it refers to (a group: its first one), with the slider over its group
void PreviewHandler::Choose(LPARAM p)
{
    if (p == -1) return;
    const int g = GroupOf(p), i = FileOf(p) == -1 ? 0 : FileOf(p);
    if (g < 0 || g >= static_cast<int>(groups.size()) || i >= static_cast<int>(groups[g].files.size())) return;
    if (g != group)
    {
        Play(false);
        group = g;
        series = groups[g].files;
        slice = -1;
    }
    GoSlice(i);
}

// Enter or a double click on a file of the tree: its folder in Explorer, with it selected
void PreviewHandler::Reveal(LPARAM p)
{
    if (p == -1) return;
    const int g = GroupOf(p), i = FileOf(p) == -1 ? 0 : FileOf(p);
    if (g < 0 || g >= static_cast<int>(groups.size()) || i >= static_cast<int>(groups[g].files.size())) return;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(groups[g].files[i].c_str(), nullptr, &pidl, 0, nullptr)))
    {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
    }
}

// One row per DICOM column: its name and its value for this file, checked if the Explorer window
// of the folder shows it (WM_READCOLUMNS)
void PreviewHandler::FillColumns()
{
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), FALSE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = const_cast<wchar_t*>(L"Column");
    c.cx = Px(170);
    ListView_InsertColumn(list, 0, &c);
    c.pszText = const_cast<wchar_t*>(L"Value");
    c.cx = Px(300);
    ListView_InsertColumn(list, 1, &c);

    filling = true;
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(g_columnKeys)); i++)
    {
        const PROPERTYKEY& key = *g_columnKeys[i];
        std::wstring name;
        IPropertyDescription* d = nullptr;
        if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&d))))
        {
            PWSTR s = nullptr;
            if (SUCCEEDED(d->GetDisplayName(&s)) && s) { name = s; CoTaskMemFree(s); }
            if (name.empty() && SUCCEEDED(d->GetCanonicalName(&s)) && s) { name = s; CoTaskMemFree(s); }
            d->Release();
        }
        if (name.empty()) name = L"DICOM column " + std::to_wstring(key.pid);
        if (name.compare(0, 6, L"DICOM ") == 0 && name.size() > 6) name = name.substr(6);   // all of them are
        LVITEMW row = {};
        row.mask = LVIF_TEXT | LVIF_PARAM;
        row.iItem = i;
        row.pszText = const_cast<wchar_t*>(name.c_str());
        row.lParam = i;
        ListView_InsertItem(list, &row);
    }
    filling = false;
    FillValues(path);
    ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
}

// The values of the columns for a file (the one shown)
void PreviewHandler::FillValues(const std::wstring& file)
{
    IPropertyStore* values = nullptr;
    if (!file.empty())
    {
        auto handler = new (std::nothrow) PropertyHandler();
        if (handler)
        {
            if (SUCCEEDED(handler->Initialize(file.c_str(), STGM_READ))) handler->QueryInterface(IID_PPV_ARGS(&values));
            handler->Release();
        }
    }
    for (int i = 0; i < ListView_GetItemCount(list) && i < static_cast<int>(ARRAYSIZE(g_columnKeys)); i++)
    {
        const PROPERTYKEY& key = *g_columnKeys[i];
        std::wstring value;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (values && SUCCEEDED(values->GetValue(key, &v)) && v.vt != VT_EMPTY)
        {
            PWSTR s = nullptr;
            if (SUCCEEDED(PSFormatForDisplayAlloc(key, v, PDFF_DEFAULT, &s)) || SUCCEEDED(PropVariantToStringAlloc(v, &s)))
            {
                value = s;
                CoTaskMemFree(s);
            }
        }
        PropVariantClear(&v);
        ListView_SetItemText(list, i, 1, const_cast<wchar_t*>(value.c_str()));
    }
    if (values) values->Release();
}

// The checks: the columns the Explorer window of the folder shows
void PreviewHandler::ReadColumns()
{
    IColumnManager* m = ColumnsOf(folder, parent);
    if (!m)
    {
        note = L"Columns: no Explorer window shows this folder";
        InvalidateRect(wnd, &statusArea, TRUE);
        return;
    }
    const std::vector<PROPERTYKEY> shown = VisibleColumns(m);
    m->Release();
    filling = true;
    for (int i = 0; i < ListView_GetItemCount(list); i++)
    {
        const PROPERTYKEY& key = *g_columnKeys[i];
        bool on = std::any_of(shown.begin(), shown.end(), [&](const PROPERTYKEY& k) { return IsEqualPropertyKey(k, key); });
        ListView_SetCheckState(list, i, on);
    }
    filling = false;
}

// A check clicked: that column is added to the end of the Explorer window, or taken out of it
void PreviewHandler::Toggle(int row)
{
    const bool on = ListView_GetCheckState(list, row) != 0;
    const PROPERTYKEY& key = *g_columnKeys[row];
    IColumnManager* m = ColumnsOf(folder, parent);
    HRESULT hr = E_FAIL;
    if (m)
    {
        std::vector<PROPERTYKEY> shown = VisibleColumns(m), next;
        for (auto& k : shown) if (!IsEqualPropertyKey(k, key)) next.push_back(k);
        if (on) next.push_back(key);
        hr = next.empty() ? E_INVALIDARG : m->SetColumns(next.data(), static_cast<UINT>(next.size()));
        m->Release();
    }
    if (FAILED(hr))
    {
        filling = true;
        ListView_SetCheckState(list, row, !on);
        filling = false;
        note = m ? L"Columns: Explorer did not change them" : L"Columns: no Explorer window shows this folder";
    }
    else note.clear();
    InvalidateRect(wnd, &statusArea, TRUE);
}

// The selected rows, "name<tab>value", to the clipboard
void PreviewHandler::CopyRows()
{
    std::wstring text;
    wchar_t buffer[4096];
    for (int i = -1; (i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0;)
    {
        ListView_GetItemText(list, i, 0, buffer, ARRAYSIZE(buffer));
        text += buffer;
        text += L"\t";
        ListView_GetItemText(list, i, 1, buffer, ARRAYSIZE(buffer));
        text += buffer;
        text += L"\r\n";
    }
    if (text.empty() || !OpenClipboard(wnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory)
    {
        memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

// The tab chosen, remembered for this folder (HKCU\Software\explorer4dicoms\Preview\Tabs)
void PreviewHandler::SelectTab(int t)
{
    if (plain || t == tab || t < 0 || t >= TabCount || (t == TabDirectory && !tree)) return;
    Settle();
    const bool headers = (t == TabHeader || t == TabTags) && (tab == TabHeader || tab == TabTags);
    LRESULT first = headers ? SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0) : 0;
    tab = t;
    if (edit && (t == TabHeader || t == TabTags))
    {
        SetWindowTextW(edit, (t == TabTags ? view.tags : view.names).c_str());
        if (headers) SendMessageW(edit, EM_LINESCROLL, 0, first);   // both have the same lines
    }
    if (!folder.empty() && t != TabDirectory)       // (the tree: only in a DICOMDIR, always first)
    {
        DWORD value = static_cast<DWORD>(t);
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\explorer4dicoms\\Preview\\Tabs", folder.c_str(), REG_DWORD,
                        &value, sizeof(value));
    }
    ShowContent();
    if (HWND content = Content()) ::SetFocus(content);
    InvalidateRect(wnd, &tabArea, TRUE);
    InvalidateRect(wnd, &statusArea, TRUE);
}

void PreviewHandler::ShowContent()
{
    if (list) ShowWindow(list, tab == TabColumns ? SW_SHOWNA : SW_HIDE);
    if (edit) ShowWindow(edit, tab == TabHeader || tab == TabTags ? SW_SHOWNA : SW_HIDE);
    if (tree) ShowWindow(tree, tab == TabDirectory ? SW_SHOWNA : SW_HIDE);
}

void PreviewHandler::Recolor()
{
    // Dark check boxes and scroll bars when Explorer is dark (themes of Windows itself)
    const bool dark = GetRValue(backColor) * 299 + GetGValue(backColor) * 587 + GetBValue(backColor) * 114 < 128000;
    if (list)
    {
        ListView_SetBkColor(list, backColor);
        ListView_SetTextBkColor(list, backColor);
        ListView_SetTextColor(list, textColor);
        SetWindowTheme(list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(list, nullptr, TRUE);
    }
    if (edit)
    {
        SetWindowTheme(edit, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        InvalidateRect(edit, nullptr, TRUE);
    }
    if (tree)
    {
        TreeView_SetBkColor(tree, backColor);
        TreeView_SetTextColor(tree, textColor);
        SetWindowTheme(tree, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(tree, nullptr, TRUE);
    }
    if (wnd) InvalidateRect(wnd, nullptr, TRUE);
}

void PreviewHandler::Place()
{
    SetWindowPos(wnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
}

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
        SIZE ext = {};
        GetTextExtentPoint32W(dc, labels[t], static_cast<int>(wcslen(labels[t])), &ext);
        tabRects[t] = { x, 0, x + ext.cx + Px(16), tabArea.bottom };
        ::SetTextColor(dc, t == tab ? textColor : Dim(60));
        RECT r = tabRects[t];
        DrawTextW(dc, labels[t], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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

    const double scale = Scale();
    if (scale > 0)
    {
        // Fitted into the image area (zoom 1, centered), or the part of it in sight: only the rows and
        // columns in the area are stretched, those from the first one in a DIB of their own
        const double left = (imageArea.left + imageArea.right) / 2.0 - viewX * scale;
        const double top = (imageArea.top + imageArea.bottom) / 2.0 - viewY * scale;
        const int x0 = (std::max)(0, static_cast<int>(std::floor((imageArea.left - left) / scale)));
        const int x1 = (std::min)(view.width, static_cast<int>(std::ceil((imageArea.right - left) / scale)));
        const int y0 = (std::max)(0, static_cast<int>(std::floor((imageArea.top - top) / scale)));
        const int y1 = (std::min)(view.height, static_cast<int>(std::ceil((imageArea.bottom - top) / scale)));
        if (x1 > x0 && y1 > y0)
        {
            const int dx = static_cast<int>(std::lround(left + x0 * scale)), dy = static_cast<int>(std::lround(top + y0 * scale));
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
                          static_cast<int>(std::lround(top + y1 * scale)) - dy, x0, 0, x1 - x0, y1 - y0,
                          view.pixels.data() + static_cast<size_t>(y0) * view.width, &bi, DIB_RGB_COLORS, SRCCOPY);
            RestoreDC(dc, saved);
        }
        // The segmentations: their mask (its colors through) and their contours
        if (hosted && poster.empty() && (maskBitmap || !outlines.empty()))
        {
            const int saved = SaveDC(dc);
            IntersectClipRect(dc, imageArea.left, imageArea.top, imageArea.right, imageArea.bottom);
            if (maskBitmap)
            {
                HDC src = CreateCompatibleDC(dc);
                HGDIOBJ kept = SelectObject(src, maskBitmap);
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(dc, static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top)),
                           static_cast<int>(std::lround(view.width * scale)), static_cast<int>(std::lround(view.height * scale)),
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
                    pts.push_back({ static_cast<LONG>(std::lround(left + (p[0] + 0.5) * scale)), static_cast<LONG>(std::lround(top + (p[1] + 0.5) * scale)) });
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
            auto at = [&](double c, double r) { return POINT{ static_cast<LONG>(std::lround(left + (c + 0.5) * scale)), static_cast<LONG>(std::lround(top + (r + 0.5) * scale)) }; };
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
        // The overlay planes of the image (with the overlay): magenta, the others in colors of their own
        if (overlay && view.imaging && !view.imaging->planes.empty() && view.imaging->planes.size() == static_cast<size_t>(view.width) * view.height)
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
                           static_cast<int>(std::lround(view.width * scale)), static_cast<int>(std::lround(view.height * scale)),
                           src, 0, 0, view.width, view.height, blend);
                RestoreDC(dc, saved);
                SelectObject(src, kept);
                DeleteDC(src);
                DeleteObject(planes);
            }
        }
        // The overlay, on the part of the image in sight
        RECT shown = { static_cast<LONG>(std::lround(left)), static_cast<LONG>(std::lround(top)),
                       static_cast<LONG>(std::lround(left + view.width * scale)), static_cast<LONG>(std::lround(top + view.height * scale)) };
        if (overlay && IntersectRect(&shown, &shown, &imageArea) && shown.right - shown.left >= Px(200) &&
            shown.bottom - shown.top >= Px(150))        // (not on a small image: it would hide it)
            PaintOverlay(dc, shown);
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
// or the series, or the files of a folder). Each one gray when there is nothing to go through.
void PreviewHandler::PaintBar(HDC dc)
{
    playRect = jumpRect = trackRect = spaceTrackRect = overlayRect = {};
    if (barArea.bottom <= barArea.top) return;
    SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
    const int middle = (barArea.top + barArea.bottom) / 2;
    const RECT rows[2] = { { barArea.left, barArea.top, barArea.right, middle }, { barArea.left, middle, barArea.right, barArea.bottom } };
    HBRUSH accent = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT)), ink = CreateSolidBrush(textColor);
    HBRUSH groove = CreateSolidBrush(Dim(30)), border = CreateSolidBrush(Dim(60)), off = CreateSolidBrush(Dim(35));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN)), oldBrush = SelectObject(dc, ink);
    SIZE ext = {};

    // The overlay, at the right of the first row (hosted: the check box of the explorer)
    if (!hosted)
    {
        const RECT& r = rows[0];
        const int cy = (r.top + r.bottom) / 2, right = r.right - Px(6);
        static const wchar_t label[] = L"Overlay";
        GetTextExtentPoint32W(dc, label, ARRAYSIZE(label) - 1, &ext);
        const int box = Px(13);
        overlayRect = { right - ext.cx - Px(6) - box, r.top, right, r.bottom };
        RECT square = { overlayRect.left, cy - box / 2, overlayRect.left + box, cy - box / 2 + box };
        if (overlay)
        {
            FillRect(dc, &square, accent);
            HPEN pen = CreatePen(PS_SOLID, Px(2), RGB(255, 255, 255));
            SelectObject(dc, pen);
            POINT check[3] = { { square.left + box * 22 / 100, cy }, { square.left + box * 42 / 100, cy + box * 22 / 100 },
                               { square.left + box * 78 / 100, cy - box * 22 / 100 } };
            Polyline(dc, check, 3);
            SelectObject(dc, GetStockObject(NULL_PEN));
            DeleteObject(pen);
        }
        else FrameRect(dc, &square, border);
        ::SetTextColor(dc, textColor);
        RECT text = { square.right + Px(6), r.top, right, r.bottom };
        DrawTextW(dc, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
                                  static_cast<double>(imageArea.bottom - imageArea.top) / view.height);
    return fit > 0 ? fit * zoom : 0;
}

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
    const double px = viewX + (pt.x - cx) / before, py = viewY + (pt.y - cy) / before;
    zoom = (std::max)(1.0, (std::min)(64.0, next));
    const double after = Scale();
    viewX = px - (pt.x - cx) / after;
    viewY = py - (pt.y - cy) / after;
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
    viewY = dragViewY - (pt.y - dragFrom.y) / scale;
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
            for (const RECT* r : { &self->playRect, &self->jumpRect, &self->trackRect, &self->spaceTrackRect, &self->overlayRect })
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
                const double top = (self->imageArea.top + self->imageArea.bottom) / 2.0 - self->viewY * scale;
                column = (pt.x - left) / scale - 0.5;
                row = (pt.y - top) / scale - 0.5;
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
