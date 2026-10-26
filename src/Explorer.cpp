// The DICOM explorer: its window, its settings, the loading, the drawing; and ExploreW (rundll32)
#include "Shell.h"

DicomExplorer::~DicomExplorer()
{
    Stop();
    StopJob();
    worker.reset();
    if (preview) { preview->Unload(); preview->Release(); }
    for (HFONT f : { font, boldFont, italicFont, boldItalicFont }) if (f) DeleteObject(f);
    if (icon) DestroyIcon(icon);
    if (smallIcon) DestroyIcon(smallIcon);
}

bool DicomExplorer::Create(const std::wstring& folder, int show, bool report)
{
    reportOnLoad = report;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"DicomStudyCountExplorer";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc = PopupProc;
    pc.hInstance = g_module;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = L"DicomStudyCountThumb";
    pc.style = CS_DROPSHADOW;
    if (!RegisterClassExW(&pc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    compact = ReadSetting(L"Compact", 1) != 0;
    hideIdentity = ReadSetting(L"HideIdentity", 1) != 0;
    hoverPreview = ReadSetting(L"HoverPreview", 1) != 0;
    treeWidth = static_cast<int>(ReadSetting(L"TreeWidth", 430));
    infoWidth = static_cast<int>(ReadSetting(L"InfoWidth", 380));
    topShare = static_cast<int>(ReadSetting(L"TopShare", 560));
    unusedShare = static_cast<int>(ReadSetting(L"UnusedShare", 250));
    wnd = CreateWindowExW(0, wc.lpszClassName, L"DICOM explorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, g_module, this);
    if (!wnd) return false;
    // Where it was last time (a size for this screen the first time)
    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(wnd, &wp);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = (work.right - work.left) * 85 / 100, h = (work.bottom - work.top) * 85 / 100;
    wp.rcNormalPosition = { static_cast<LONG>(ReadSetting(L"Left", work.left + (work.right - work.left - w) / 2)),
                            static_cast<LONG>(ReadSetting(L"Top", work.top + (work.bottom - work.top - h) / 2)), 0, 0 };
    wp.rcNormalPosition.right = wp.rcNormalPosition.left + static_cast<LONG>(ReadSetting(L"Width", w));
    wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + static_cast<LONG>(ReadSetting(L"Height", h));
    wp.showCmd = ReadSetting(L"Maximized", 0) ? SW_SHOWMAXIMIZED : (show == SW_HIDE || show == SW_SHOWMINIMIZED || show == SW_MINIMIZE ? show : SW_SHOWNORMAL);
    SetWindowPlacement(wnd, &wp);
    UpdateWindow(wnd);
    std::wstring start = folder;
    if (start.empty()) start = ReadText(L"Folder");
    if (start.empty() || !PathIsDirectoryW(start.c_str())) Browse();
    else Load(start);
    return true;
}

// Enter in the boxes of the folder and of the search, F3, Ctrl+C in the lists
bool DicomExplorer::PreTranslate(MSG& m)
{
    if (m.message != WM_KEYDOWN) return false;
    if (m.wParam == VK_ESCAPE && job && !job->cancel)
    {
        job->cancel = true;
        SetStatus(L"Stopping...");
        return true;
    }
    if (m.wParam == VK_RETURN && m.hwnd == pathBox)
    {
        Load(TextOf(pathBox));
        return true;
    }
    if ((m.wParam == VK_RETURN && m.hwnd == findBox) || m.wParam == VK_F3)
    {
        FindNext((GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return true;
    }
    if ((m.hwnd == info || m.hwnd == full) && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (m.wParam == 'C' || m.wParam == VK_INSERT) { CopyRows(m.hwnd, true); return true; }
        if (m.wParam == 'A') { ListView_SetItemState(m.hwnd, -1, LVIS_SELECTED, LVIS_SELECTED); return true; }
    }
    if (m.hwnd == tree && (GetKeyState(VK_CONTROL) & 0x8000) && m.wParam == 'C' && selected > 0)
    {
        CopyPaths(selected);
        return true;
    }
    return false;
}

// ----- settings

DWORD DicomExplorer::ReadSetting(const wchar_t* name, DWORD fallback)
{
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, Settings, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

void DicomExplorer::WriteSetting(const wchar_t* name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, Settings, name, REG_DWORD, &value, sizeof(value));
}

std::wstring DicomExplorer::ReadText(const wchar_t* name)
{
    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    return RegGetValueW(HKEY_CURRENT_USER, Settings, name, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS ? buffer : L"";
}

void DicomExplorer::SaveSettings()
{
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (GetWindowPlacement(wnd, &wp))
    {
        WriteSetting(L"Left", static_cast<DWORD>(wp.rcNormalPosition.left));
        WriteSetting(L"Top", static_cast<DWORD>(wp.rcNormalPosition.top));
        WriteSetting(L"Width", static_cast<DWORD>(wp.rcNormalPosition.right - wp.rcNormalPosition.left));
        WriteSetting(L"Height", static_cast<DWORD>(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top));
        WriteSetting(L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    }
    WriteSetting(L"TreeWidth", static_cast<DWORD>(treeWidth));
    WriteSetting(L"InfoWidth", static_cast<DWORD>(infoWidth));
    WriteSetting(L"TopShare", static_cast<DWORD>(topShare));
    WriteSetting(L"UnusedShare", static_cast<DWORD>(unusedShare));
}

int DicomExplorer::Px(int n) const
{
    const UINT dpi = wnd ? GetDpiForWindow(wnd) : 96;
    return MulDiv(n, dpi ? dpi : 96, 96);
}

std::wstring DicomExplorer::TextOf(HWND h)
{
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)) + 1, L'\0');
    s.resize(GetWindowTextW(h, &s[0], static_cast<int>(s.size())));
    return s;
}

// ----- the window

void DicomExplorer::CreateControls()
{
    auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, wnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_module, nullptr);
    };
    pathBox = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IdPath);
    browse = make(0, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, IdBrowse);
    reload = make(0, L"BUTTON", L"Reload", WS_TABSTOP | BS_PUSHBUTTON, IdReload);
    reportButton = make(0, L"BUTTON", L"Report", WS_TABSTOP | BS_PUSHBUTTON, IdReport);
    findLabel = make(0, L"STATIC", L"Find:", SS_RIGHT | SS_CENTERIMAGE, 0);
    findBox = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IdFind);
    compactBox = make(0, L"BUTTON", L"Compact", WS_TABSTOP | BS_AUTOCHECKBOX, IdCompact);
    hoverBox = make(0, L"BUTTON", L"Hover preview", WS_TABSTOP | BS_AUTOCHECKBOX, IdHover);
    navigatorBox = make(0, L"BUTTON", L"Navigator", WS_TABSTOP | BS_AUTOCHECKBOX, IdNavigator);
    overlayBox = make(0, L"BUTTON", L"Overlay", WS_TABSTOP | BS_AUTOCHECKBOX, IdOverlay);
    refBox = make(0, L"BUTTON", L"Ref lines", WS_TABSTOP | BS_AUTOCHECKBOX, IdRefLines);
    segBox = make(0, L"BUTTON", L"Segmentations", WS_TABSTOP | BS_AUTOCHECKBOX, IdSegments);
    showSegments = ReadSetting(L"Segmentations", 1) != 0;
    Button_SetCheck(segBox, showSegments ? BST_CHECKED : BST_UNCHECKED);
    refLines = ReadSetting(L"ReferenceLines", 1) != 0;
    Button_SetCheck(refBox, refLines ? BST_CHECKED : BST_UNCHECKED);
    identityBox = make(0, L"BUTTON", L"Hide patient identity", WS_TABSTOP | BS_AUTOCHECKBOX, IdIdentity);
    // (no tooltips of its own: the floating thumbnail writes the line of the node)
    tree = make(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                TVS_SHOWSELALWAYS | TVS_NOTOOLTIPS, 0);
    SetWindowSubclass(tree, TreeProc, 1, reinterpret_cast<DWORD_PTR>(this));
    unusedLabel = make(0, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0);
    unusedTree = make(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                      TVS_SHOWSELALWAYS | TVS_NOTOOLTIPS, 0);
    SetWindowSubclass(unusedTree, TreeProc, 1, reinterpret_cast<DWORD_PTR>(this));
    popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                            L"DicomStudyCountThumb", L"", WS_POPUP | WS_BORDER, 0, 0, 10, 10, wnd, nullptr, g_module, this);
    if (popup) SetLayeredWindowAttributes(popup, 0, 255, LWA_ALPHA);
    worker = std::make_unique<ThumbWorker>(wnd, WM_THUMB);
    info = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER, 0);
    filterLabel = make(0, L"STATIC", L"Full info", SS_LEFT | SS_CENTERIMAGE, 0);
    filterBox = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IdFilter);
    full = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0);
    SetWindowSubclass(full, FullProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(ListView_GetHeader(full), HeaderProc, 1, reinterpret_cast<DWORD_PTR>(this));
    status = make(0, STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, 0);
    SendMessageW(findBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"text or regex; Enter / F3: next"));
    SendMessageW(filterBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter: tag, name or value (text or regex)"));
    Button_SetCheck(compactBox, compact ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(identityBox, hideIdentity ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hoverBox, hoverPreview ? BST_CHECKED : BST_UNCHECKED);
    navigator.hovered = [this](int i) { Light(i); };
    if (plot.Create(wnd))
    {
        plot.closed = [this]() { plotTag = 0; Layout(); };
        plot.picked = [this](int i) { PlotPicked(i); };
    }
    navigator.picked = [this](int i) { Pick(i); };
    navigator.aimed = [this](bool on, const double* from, const double* toward) { Aimed(on, from, toward); };
    if (navigator.Create(wnd) && ReadSetting(L"Navigator", 0)) { Button_SetCheck(navigatorBox, BST_CHECKED); navigator.Show(true); }
    SetWindowTheme(tree, L"Explorer", nullptr);
    SetWindowTheme(unusedTree, L"Explorer", nullptr);
    TreeView_SetExtendedStyle(unusedTree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    UnusedLabel();
    // (check boxes, with a third state, partly on: what the navigator draws; set after the tree is made, as its
    // documentation asks, the partial one first: the images of the boxes are made with the style)
    TreeView_SetExtendedStyle(tree, TVS_EX_PARTIALCHECKBOXES, TVS_EX_PARTIALCHECKBOXES);
    SetWindowLongPtrW(tree, GWL_STYLE, GetWindowLongPtrW(tree, GWL_STYLE) | TVS_CHECKBOXES);
    CheckImages();
    TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    for (HWND list : { info, full })
    {
        SetWindowTheme(list, L"Explorer", nullptr);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    }
    auto column = [&](HWND list, int at, const wchar_t* title, int width) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(title);
        c.cx = Px(width);
        ListView_InsertColumn(list, at, &c);
    };
    column(info, 0, L"Node", 130);
    column(info, 1, L"", 400);
    column(full, 0, L"Tag", 95);
    TagColumn(ReadSetting(L"TagColumn", 0) != 0);
    column(full, 1, L"Attribute", 230);
    column(full, 2, L"Value", 600);
    ListView_EnableGroupView(full, TRUE);
    Refont();
    // The image: the preview, alone
    preview = new (std::nothrow) PreviewHandler();
    if (preview)
    {
        preview->moved = [this](int i) { ShownFile(i); Live(i); Followed(i); };
        preview->running = [this](bool on) { if (!on) navigator.SetLive(-1, 0, 0, nullptr); };
        preview->pointed = [this](bool on, double column, double row) { Pointed(on, column, row); };
        preview->Host(wnd, imageRect);
        Button_SetCheck(overlayBox, preview->Overlay() ? BST_CHECKED : BST_UNCHECKED);
        preview->ShowFiles({}, -1);
    }
}

void DicomExplorer::Refont()
{
    for (HFONT* f : { &font, &boldFont }) if (*f) { DeleteObject(*f); *f = nullptr; }
    const int points9 = -MulDiv(9, GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96, 72);
    font = CreateFontW(points9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    boldFont = CreateFontW(points9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    for (HFONT* f : { &italicFont, &boldItalicFont }) if (*f) { DeleteObject(*f); *f = nullptr; }
    italicFont = CreateFontW(points9, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    boldItalicFont = CreateFontW(points9, 0, 0, 0, FW_SEMIBOLD, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    for (HWND h : { pathBox, browse, reload, reportButton, findLabel, findBox, compactBox, hoverBox, navigatorBox, overlayBox, refBox, segBox, identityBox, tree, unusedTree, info, filterLabel,
                    filterBox, full, status })
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND h : { filterLabel, unusedLabel }) if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont), TRUE);
}

// The folder and the options at the top, the tree at the left; at the right the node and its image,
// and under them its full info; the status bar at the bottom. The bars between them move.
void DicomExplorer::Layout()
{
    if (!status) return;
    RECT c;
    GetClientRect(wnd, &c);
    SendMessageW(status, WM_SIZE, 0, 0);
    RECT s = {};
    GetWindowRect(status, &s);
    const int m = Px(6), gap = Px(6), line = Px(25), w = c.right, bottom = c.bottom - (s.bottom - s.top) - m;
    int x = m;
    const int y = m;
    int right = w - m;
    auto put = [&](HWND h, int left, int width) { MoveWindow(h, left, y, width, line, TRUE); };
    put(identityBox, right - Px(150), Px(150));
    right -= Px(150) + gap;
    put(hoverBox, right - Px(108), Px(108));
    right -= Px(108) + gap;
    put(navigatorBox, right - Px(84), Px(84));
    right -= Px(84) + gap;
    put(segBox, right - Px(108), Px(108));
    right -= Px(108) + gap;
    put(refBox, right - Px(80), Px(80));
    right -= Px(80) + gap;
    put(overlayBox, right - Px(74), Px(74));
    right -= Px(74) + gap;
    put(compactBox, right - Px(80), Px(80));
    right -= Px(80) + gap * 2;
    put(reportButton, right - Px(76), Px(76));
    right -= Px(76) + Px(4);
    put(reload, right - Px(70), Px(70));
    right -= Px(70) + Px(4);
    put(browse, right - Px(80), Px(80));
    right -= Px(80) + Px(4);
    put(pathBox, x, (std::max)(Px(100), right - x));

    const int top = y + line + m;
    const int width = w - 2 * m;
    int tw = (std::max)(Px(150), (std::min)(Px(treeWidth), width - Px(300)));
    // (the tree, and the unused one under it)
    trees = { m, top, m + tw, bottom };
    // (and Find under them)
    const int ch = bottom - top - gap - line - Px(4) - line;
    const int uh = (std::max)(Px(40), (std::min)(ch - Px(80), static_cast<int>(static_cast<long long>(ch) * unusedShare / 1000)));
    const int mh = ch - uh;
    MoveWindow(tree, m, top, tw, mh, TRUE);
    unusedSplit = { m, top + mh, m + tw, top + mh + gap };
    MoveWindow(unusedLabel, m, top + mh + gap, tw, line, TRUE);
    MoveWindow(unusedTree, m, top + mh + gap + line, tw, uh, TRUE);
    MoveWindow(findLabel, m, bottom - line, Px(34), line, TRUE);
    MoveWindow(findBox, m + Px(38), bottom - line, tw - Px(38), line, TRUE);
    treeSplit = { m + tw, top, m + tw + gap, bottom };
    const int x0 = m + tw + gap, x1 = w - m, h = bottom - top;
    int th = static_cast<int>(static_cast<long long>(h - gap - line) * topShare / 1000);
    th = (std::max)(Px(80), (std::min)(th, h - gap - line - Px(60)));
    int iw = (std::max)(Px(120), (std::min)(Px(infoWidth), x1 - x0 - gap - Px(150)));
    MoveWindow(info, x0, top, iw, th, TRUE);
    ListView_SetColumnWidth(info, 1, (std::max)(Px(100), iw - ListView_GetColumnWidth(info, 0) - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(wnd)) - Px(4)));
    infoSplit = { x0 + iw, top, x0 + iw + gap, top + th };
    imageRect = { x0 + iw + gap, top, x1, top + th };
    if (preview) preview->SetRect(&imageRect);
    topSplit = { x0, top + th, x1, top + th + gap };
    const int fy = top + th + gap;
    // (the navigator, if shown, at the right of the full info: half of it each; the filter under the list)
    const bool navigating = navigator.Visible();
    const int fw = navigating ? (x1 - x0 - gap) / 2 : x1 - x0;
    MoveWindow(filterLabel, x0, fy, Px(70), line, TRUE);
    const int listTop = fy + line + Px(4), listBottom = bottom - line - Px(4);
    if (plot.Visible())
    {
        const int ph = (listBottom - listTop) * 45 / 100;
        MoveWindow(full, x0, listTop, fw, listBottom - ph - gap - listTop, TRUE);
        plot.Move({ x0, listBottom - ph, x0 + fw, listBottom });
    }
    else MoveWindow(full, x0, listTop, fw, listBottom - listTop, TRUE);
    MoveWindow(filterBox, x0, bottom - line, fw, line, TRUE);
    if (navigating) navigator.Move({ x0 + fw + gap, fy, x1, bottom });
    InvalidateRect(wnd, nullptr, TRUE);
}

void DicomExplorer::SetStatus(const std::wstring& text) { if (status) SetWindowTextW(status, text.c_str()); }

// ----- loading

void DicomExplorer::Browse()
{
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD options = 0;
    d->GetOptions(&options);
    d->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"DICOM explorer: the folder to explore");
    if (!root.empty())
    {
        IShellItem* at = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&at)))) { d->SetFolder(at); at->Release(); }
    }
    if (SUCCEEDED(d->Show(wnd)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item)))
        {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { Load(p); CoTaskMemFree(p); }
            item->Release();
        }
    }
    d->Release();
}

void DicomExplorer::Stop()
{
    if (data && data->thread.joinable())
    {
        data->cancel = true;
        data->thread.join();
    }
    if (wnd) KillTimer(wnd, TimerProgress);
}

void DicomExplorer::Load(std::wstring folder)
{
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    if (!PathIsDirectoryW(folder.c_str()))
    {
        SetStatus(L"Not a folder: " + folder);
        return;
    }
    Stop();
    root = folder;
    SetWindowTextW(pathBox, root.c_str());
    SetWindowTextW(wnd, (L"DICOM explorer - " + root).c_str());
    RegSetKeyValueW(HKEY_CURRENT_USER, Settings, L"Folder", REG_SZ, root.c_str(),
                    static_cast<DWORD>((root.size() + 1) * sizeof(wchar_t)));
    Clear();
    InitDcmtk();
    data = std::make_unique<Gathering>();
    data->root = root;
    data->window = wnd;
    data->message = WM_GATHERED;
    data->thread = std::thread(Gather, data.get());
    SetTimer(wnd, TimerProgress, 200, nullptr);
    Progress();
}

void DicomExplorer::Clear()
{
    HidePopup();
    thumbs.clear();
    posters.clear();
    generation++;
    hoverItem = nullptr;
    ready = false;
    selected = sliderNode = -1;
    following = true;                           // (no selection events while the tree empties)
    TreeView_DeleteAllItems(tree);
    TreeView_DeleteAllItems(unusedTree);
    following = false;
    fileUnused.clear();
    usedCount.clear();
    unusedItem.clear();
    unusedFilled.clear();
    openMemory.clear();
    UnusedLabel();
    navPlanes.clear();
    filePlane.clear();
    planesMade = false;
    navigated = -1;
    navPatient = -1;
    navigator.Clear();
    navScene.clear();
    lit.clear();
    overlays.clear();
    segmentedCount.clear();
    overlayCount.clear();
    fileOn.clear();
    onCount.clear();
    nodes.clear();
    fileNode.clear();
    ListView_DeleteAllItems(info);
    ListView_DeleteAllItems(full);
    ListView_RemoveAllGroups(full);
    if (preview) preview->ShowFiles({}, -1);
}

void DicomExplorer::Progress()
{
    if (!data) return;
    wchar_t text[160];
    if (data->listing) swprintf_s(text, L"Looking for files: %ld so far...", data->found.load());
    else swprintf_s(text, L"Reading the files: %ld of %ld...", data->done.load(), data->found.load());
    SetStatus(text);
}

void DicomExplorer::Finished()
{
    KillTimer(wnd, TimerProgress);
    if (!data) return;
    if (data->thread.joinable()) data->thread.join();
    Build();
    fileOn.assign(data->files.size(), 1);
    overlays.clear();
    for (size_t f = 0; f < data->files.size(); f++)
    {
        const Gathered& g = data->files[f];
        if (g.frame > 0) continue;                  // (an enhanced file once)
        const std::wstring* c = AttributeOf(static_cast<int>(f), 0x00080016);
        if (!c) continue;
        const bool seg = *c == Widen(SegSopClass), rt = *c == Widen(RtStructSopClass);
        if (!seg && !rt) continue;
        Overlay o;
        o.path = g.path;
        o.rt = rt;
        o.study = g.study;
        overlays.push_back(std::move(o));
    }
    MarkSegmented();
    MarkOverlays();
    derivedCount.assign(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); i++) for (int f : nodes[i].files) derivedCount[i] += fileDerived[f];
    onCount.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) onCount[i] = nodes[i].files.size();
    fileUnused.assign(data->files.size(), 0);
    usedCount = onCount;
    unusedItem.assign(nodes.size(), nullptr);
    unusedFilled.assign(nodes.size(), 0);
    ready = true;
    ComputeShown();
    FillTree();
    long series = 0, studies = 0, patients = static_cast<long>(nodes.empty() ? 0 : nodes[0].children.size()), troubled = 0;
    for (const auto& n : nodes)
    {
        if (n.level == Level::Study) studies++;
        if (n.level == Level::Series) { series++; if (!n.flags.empty()) troubled++; }
    }
    wchar_t text[400];
    swprintf_s(text, L"%zu DICOM files: %ld patient%s, %ld stud%s, %ld series", nodes.empty() ? size_t(0) : FilesOf(0), patients,
               patients == 1 ? L"" : L"s", studies, studies == 1 ? L"y" : L"ies", series);
    std::wstring s = text;
    if (data->others) s += L"   -   " + std::to_wstring(data->others) + L" other files";
    if (data->directories) s += L"   -   " + std::to_wstring(data->directories) + L" DICOMDIR";
    if (data->offline) s += L"   -   " + std::to_wstring(data->offline) + L" not downloaded (not read)";
    if (troubled) s += L"   -   \u26A0 " + std::to_wstring(troubled) + L" series with problems";
    swprintf_s(text, L"   -   %.1f s", data->seconds);
    summary = s + text;
    SetStatus(summary);
    if (!nodes.empty() && !nodes[0].shown.empty()) TreeView_SelectItem(tree, nodes[nodes[0].shown[0]].item);
    // (asked for from the menu of a folder: its report, at once)
    if (reportOnLoad)
    {
        reportOnLoad = false;
        Report(0);
    }
}

// ----- drawing

COLORREF DicomExplorer::LevelColor(int level)
{
    switch (static_cast<Level>(level))
    {
    case Level::Patient: return RGB(192, 0, 0);
    case Level::Study: return RGB(0, 128, 0);
    case Level::Series: return RGB(0, 70, 220);
    case Level::Orientation: return RGB(165, 42, 42);
    case Level::Position: return RGB(170, 0, 170);
    default: return GetSysColor(COLOR_WINDOWTEXT);
    }
}

LRESULT DicomExplorer::Notify(NMHDR* h)
{
    if (h->hwndFrom == tree || h->hwndFrom == unusedTree)
    {
        const bool unused = h->hwndFrom == unusedTree;
        switch (h->code)
        {
        case TVN_ITEMEXPANDINGW:
        {
            auto tv = reinterpret_cast<NMTREEVIEWW*>(h);
            if (tv->action & TVE_EXPAND)
            {
                if (unused) InsertUnused(static_cast<int>(tv->itemNew.lParam));
                else Insert(static_cast<int>(tv->itemNew.lParam));
            }
            FoundAgain();
            return FALSE;
        }
        case TVN_SELCHANGEDW:
            if (!following && ready) Choose(static_cast<int>(reinterpret_cast<NMTREEVIEWW*>(h)->itemNew.lParam));
            return 0;
        case NM_DBLCLK:
        case NM_RETURN:
            if (selected > 0 && nodes[selected].level == Level::Image)
            {
                ShowInExplorer(PathsOf(selected));
                return TRUE;
            }
            return 0;
        case NM_CUSTOMDRAW:
        {
            auto cd = reinterpret_cast<NMTVCUSTOMDRAW*>(h);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && ready)
            {
                const int n = static_cast<int>(cd->nmcd.lItemlParam);
                if (n <= 0 || n >= static_cast<int>(nodes.size())) return CDRF_DODEFAULT;
                if (!(cd->nmcd.uItemState & CDIS_SELECTED))
                {
                    cd->clrText = LevelColor(static_cast<int>(nodes[n].level));
                    if (n < static_cast<int>(derivedCount.size()) && derivedCount[n] > 0 && derivedCount[n] < nodes[n].files.size())
                        cd->clrTextBk = RGB(228, 228, 228);     // (derived and original images under it)
                    if (Found(n, unused)) cd->clrTextBk = RGB(255, 236, 110);
                }
                // (the nodes of the plane under the mouse in the navigator: a black frame around them, after)
                const LRESULT post = !unused && std::find(lit.begin(), lit.end(), n) != lit.end() ? CDRF_NOTIFYPOSTPAINT : 0;
                // (bold: series and patients; all of it derived: italic, and not bold)
                const bool italic = n < static_cast<int>(derivedCount.size()) && derivedCount[n] > 0 && derivedCount[n] == nodes[n].files.size();
                const bool bold = !italic && (nodes[n].level == Level::Series || nodes[n].level == Level::Patient);
                if (bold || italic)
                {
                    SelectObject(cd->nmcd.hdc, bold ? (italic ? boldItalicFont : boldFont) : italicFont);
                    return CDRF_NEWFONT | post;
                }
                return CDRF_DODEFAULT | post;
            }
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT)
            {
                RECT r;
                if (TreeView_GetItemRect(tree, reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec), &r, TRUE))
                {
                    InflateRect(&r, Px(2), 0);
                    HPEN pen = CreatePen(PS_INSIDEFRAME, Px(2), RGB(0, 0, 0));
                    HGDIOBJ oldPen = SelectObject(cd->nmcd.hdc, pen);
                    HGDIOBJ oldBrush = SelectObject(cd->nmcd.hdc, GetStockObject(NULL_BRUSH));
                    Rectangle(cd->nmcd.hdc, r.left, r.top, r.right, r.bottom);
                    SelectObject(cd->nmcd.hdc, oldBrush);
                    SelectObject(cd->nmcd.hdc, oldPen);
                    DeleteObject(pen);
                }
            }
            return CDRF_DODEFAULT;
        }
        }
    }
    if (h->hwndFrom == info && h->code == NM_CUSTOMDRAW)
    {
        auto cd = reinterpret_cast<NMLVCUSTOMDRAW*>(h);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && static_cast<int>(cd->nmcd.dwItemSpec) >= firstProblemRow)
            cd->clrText = ProblemColor;
        return CDRF_DODEFAULT;
    }
    if (h->hwndFrom == full && h->code == NM_DBLCLK)
    {
        const int row = reinterpret_cast<NMITEMACTIVATE*>(h)->iItem;
        if (row >= 0) PlotRow(row);
        return 0;
    }
    if (h->hwndFrom == full && h->code == NM_CUSTOMDRAW)
    {
        auto cd = reinterpret_cast<NMLVCUSTOMDRAW*>(h);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
        {
            const int kind = static_cast<int>(cd->nmcd.lItemlParam);
            cd->clrText = kind == 100 ? RGB(110, 110, 110) : LevelColor(kind);
            // (the Tag column folded: its text not seen - it is still copied)
            if (cd->iSubItem == 0 && !tagOpen) cd->clrText = cd->clrTextBk = GetSysColor(COLOR_WINDOW);
            return CDRF_DODEFAULT;
        }
    }
    return 0;
}

DicomExplorer::Split DicomExplorer::SplitAt(POINT pt) const
{
    if (PtInRect(&treeSplit, pt)) return Split::Tree;
    if (PtInRect(&unusedSplit, pt)) return Split::Unused;
    if (PtInRect(&infoSplit, pt)) return Split::Info;
    if (PtInRect(&topSplit, pt)) return Split::Top;
    return Split::None;
}

void DicomExplorer::DragTo(POINT pt)
{
    const UINT dpi = GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96;
    const RECT& treeRect = trees;               // (both trees)
    if (split == Split::Tree) treeWidth = MulDiv(pt.x - treeRect.left, 96, dpi);
    else if (split == Split::Info) infoWidth = MulDiv(pt.x - treeSplit.right, 96, dpi);
    else if (split == Split::Unused)
    {
        const int h = treeRect.bottom - treeRect.top - Px(6) - Px(25);
        if (h > 0) unusedShare = (std::max)(50, (std::min)(900, static_cast<int>(static_cast<long long>(treeRect.bottom - pt.y - Px(25)) * 1000 / h)));
    }
    else if (split == Split::Top)
    {
        const int h = treeRect.bottom - treeRect.top - Px(6) - Px(25);
        if (h > 0) topShare = (std::max)(50, (std::min)(950, static_cast<int>(static_cast<long long>(pt.y - treeRect.top) * 1000 / h)));
    }
    Layout();
}

LRESULT CALLBACK DicomExplorer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<DicomExplorer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        self = static_cast<DicomExplorer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->wnd = hwnd;
        break;
    case WM_CREATE:
        self->icon = DrawIcon(GetSystemMetrics(SM_CXICON), 16, L"D");
        self->smallIcon = DrawIcon(GetSystemMetrics(SM_CXSMICON), 16, L"D");
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(self->icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(self->smallIcon));
        self->CreateControls();
        return 0;
    case WM_SIZE:
        if (self) self->Layout();
        return 0;
    case WM_DPICHANGED:
    {
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (self) { self->Refont(); self->Layout(); }
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = { 800, 500 };
        return 0;
    case WM_GATHERED:
        if (self) self->Finished();
        return 0;
    case WM_THUMB:
        if (self) self->Thumbnailed(reinterpret_cast<ThumbDone*>(lp));
        else delete reinterpret_cast<ThumbDone*>(lp);
        return 0;
    case WM_EXPORTED:
        if (self) self->JobDone();
        return 0;
    case WM_ACTIVATEAPP:
        if (self && !wp) self->HidePopup();
        break;
    case WM_TIMER:
        if (!self) return 0;
        if (wp == TimerJob) { self->JobProgress(); return 0; }
        if (wp != TimerProgress) KillTimer(hwnd, wp);
        if (wp == TimerProgress) self->Progress();
        else if (wp == TimerFind) self->FindChanged();
        else if (wp == TimerFilter)
        {
            self->filterText = TextOf(self->filterBox);
            Compile(self->filterText, self->filterRegex, self->filterIsRegex);
            if (self->selected > 0) self->FillFull(self->selected);
        }
        else if (wp == TimerFollow && self->selected > 0)
        {
            self->FillInfo(self->selected);
            self->FillFull(self->selected);
            self->Navigate(self->selected);
        }
        return 0;
    case WM_COMMAND:
        if (!self) break;
        switch (LOWORD(wp))
        {
        case IdBrowse: self->Browse(); return 0;
        case IdReload: if (!self->root.empty()) self->Load(self->root); return 0;
        case IdReport: self->Report(0); return 0;
        case IdHover:
            self->hoverPreview = Button_GetCheck(self->hoverBox) == BST_CHECKED;
            WriteSetting(L"HoverPreview", self->hoverPreview ? 1 : 0);
            if (!self->hoverPreview) self->HidePopup();
            return 0;
        case IdNavigator:
        {
            const bool on = Button_GetCheck(self->navigatorBox) == BST_CHECKED;
            WriteSetting(L"Navigator", on ? 1 : 0);
            self->navigator.Show(on);
            self->Layout();
            if (on && self->selected > 0) self->Navigate(self->selected);
            return 0;
        }
        case IdSegments:
            self->showSegments = Button_GetCheck(self->segBox) == BST_CHECKED;
            WriteSetting(L"Segmentations", self->showSegments ? 1 : 0);
            self->Segments();
            return 0;
        case IdRefLines:
            self->refLines = Button_GetCheck(self->refBox) == BST_CHECKED;
            WriteSetting(L"ReferenceLines", self->refLines ? 1 : 0);
            self->Guides(-1);
            return 0;
        case IdOverlay:
            if (self->preview) self->preview->SetOverlay(Button_GetCheck(self->overlayBox) == BST_CHECKED);
            return 0;
        case IdCompact:
            self->compact = Button_GetCheck(self->compactBox) == BST_CHECKED;
            WriteSetting(L"Compact", self->compact ? 1 : 0);
            self->Recompact();
            return 0;
        case IdIdentity:
            self->hideIdentity = Button_GetCheck(self->identityBox) == BST_CHECKED;
            WriteSetting(L"HideIdentity", self->hideIdentity ? 1 : 0);
            if (self->selected > 0) { self->FillInfo(self->selected); self->FillFull(self->selected); }
            return 0;
        case IdFind:
            if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, TimerFind, 250, nullptr);
            return 0;
        case IdFilter:
            if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, TimerFilter, 250, nullptr);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (self) return self->Notify(reinterpret_cast<NMHDR*>(lp));
        break;
    case WM_CONTEXTMENU:
        if (self && reinterpret_cast<HWND>(wp) == self->full) { self->FullMenu(lp); return 0; }
        if (self && reinterpret_cast<HWND>(wp) == self->tree) { self->TreeMenu(lp, false); return 0; }
        if (self && reinterpret_cast<HWND>(wp) == self->unusedTree) { self->TreeMenu(lp, true); return 0; }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_SETCURSOR:
        if (self && LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const Split s = self->SplitAt(pt);
            if (s != Split::None) { SetCursor(LoadCursor(nullptr, s == Split::Top || s == Split::Unused ? IDC_SIZENS : IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN:
        if (self)
        {
            self->split = self->SplitAt({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (self->split != Split::None) SetCapture(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (self && self->split != Split::None) self->DragTo({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;
    case WM_LBUTTONUP:
        if (self && self->split != Split::None) { self->split = Split::None; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        if (self) self->split = Split::None;
        return 0;
    case WM_CLOSE:
        if (self) self->SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (self)
        {
            self->Stop();
            self->StopJob();
            self->worker.reset();
        }
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// rundll32 "...\DicomShell.dll",Explore "<folder>": the explorer, until its window is closed. Without a
// folder: the last one, or one to choose.
extern "C" void CALLBACK ExploreW(HWND, HINSTANCE, LPWSTR line, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // The themed controls (common controls 6): rundll32 has no manifest asking for them
    ULONG_PTR cookie = 0;
    HANDLE context = ThemedControls();
    const bool active = context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    std::wstring folder = line ? line : L"";
    // "/report <folder>": its report too
    const bool report = _wcsnicmp(folder.c_str(), L"/report", 7) == 0;
    if (report) folder.erase(0, 7);
    while (!folder.empty() && (folder.back() == L' ' || folder.back() == L'"')) folder.pop_back();
    while (!folder.empty() && (folder.front() == L' ' || folder.front() == L'"')) folder.erase(0, 1);
    // A file (a DICOMDIR...): its folder
    if (!folder.empty() && !PathIsDirectoryW(folder.c_str()) && PathFileExistsW(folder.c_str()))
        folder = folder.substr(0, folder.find_last_of(L'\\'));
    {
        DicomExplorer explorer;
        if (explorer.Create(folder, show, report))
        {
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0)
            {
                // The wheel to the panel under the mouse, not to the one with the focus
                if (m.message == WM_MOUSEWHEEL || m.message == WM_MOUSEHWHEEL)
                {
                    const POINT at = { GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam) };
                    HWND under = WindowFromPoint(at);
                    if (under && under != m.hwnd && GetWindowThreadProcessId(under, nullptr) == GetCurrentThreadId()) m.hwnd = under;
                }
                if (explorer.PreTranslate(m)) continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
    if (active) DeactivateActCtx(0, cookie);
    if (SUCCEEDED(com)) CoUninitialize();
}
