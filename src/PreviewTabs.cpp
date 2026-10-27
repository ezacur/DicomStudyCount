// The preview: its tabs - the DICOM columns (checked as the Explorer window shows them), the header, the
// DICOMDIR tree - and their colors
#include "Shell.h"

// The DICOM columns, as the tab "Columns" of the preview lists them: the order of Properties > Details
static const PROPERTYKEY* const g_columnKeys[] = {
    &PKEY_StudyHash, &PKEY_SeriesHash, &PKEY_Modality, &PKEY_SeriesNumber, &PKEY_InstanceNumber, &PKEY_SeriesDescription,
    &PKEY_Protocol, &PKEY_Plane, &PKEY_SeriesKind, &PKEY_Segmentation, &PKEY_Acquired, &PKEY_TriggerTime, &PKEY_ZLevel, &PKEY_Rows, &PKEY_Columns,
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
    // (the check boxes: which of them Explorer shows; not in the Lister of Total Commander)
    ListView_SetExtendedListViewStyle(list, (listerWindow ? 0 : LVS_EX_CHECKBOXES) | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
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
        if (&key == &PKEY_Segmentation)        // (as the image shows it: the folders around are read in the background)
            value = maskBitmap || !outlines.empty() || (view.imaging && !view.imaging->planes.empty()) ? L"seg" : L"";
        else if (values && SUCCEEDED(values->GetValue(key, &v)) && v.vt != VT_EMPTY)
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
