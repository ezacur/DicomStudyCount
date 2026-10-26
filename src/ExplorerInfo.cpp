// The DICOM explorer: the info of a node, its full info, the plot of an attribute
#include "Shell.h"

// ----- the navigator

// What the node is: a few values of it, and how many of what are under it
void DicomExplorer::FillInfo(int n)
{
    const GatherNode& node = nodes[n];
    const std::vector<Gathered>& files = data->files;
    const Gathered& g = files[node.files[0]];
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    auto row = [&](const std::wstring& name, const std::wstring& value) { if (!value.empty()) rows.push_back({ name, value }); };
    auto value = [&](Uint32 tag) { return Value(n, tag); };
    auto count = [&](Level level) {
        size_t k = 0;
        std::function<void(int)> walk = [&](int i) { if (nodes[i].level == level) { k++; return; } for (int c : nodes[i].children) walk(c); };
        walk(n);
        return k;
    };
    static const wchar_t* const levels[] = { L"All", L"Patient", L"Study", L"Series", L"Orientation", L"Position", L"Image" };
    row(levels[static_cast<int>(node.level)], node.text);
    long long bytes = 0;
    long frames = 0;
    std::unordered_set<std::wstring> folders;
    for (int f : node.files)
    {
        bytes += files[f].bytes;
        frames += files[f].frames;
        folders.insert(files[f].path.substr(0, files[f].path.find_last_of(L'\\')));
    }
    wchar_t buffer[160];
    switch (node.level)
    {
    case Level::Patient:
        row(L"Patient ID", Widen(g.patient, g.charset));
        if (!node.person.empty())
            row(L"Person", L"[" + node.person + L"]  (" + std::to_wstring(node.sharing) + L" people have this Patient ID; this hash is of the name and birth date)");
        row(L"Studies", std::to_wstring(count(Level::Study)));
        row(L"Series", std::to_wstring(count(Level::Series)));
        break;
    case Level::Study:
        row(L"Date", DateTimeText(g.studyDate, g.studyTime));
        row(L"Description", value(0x00081030));
        row(L"Accession number", value(0x00080050));
        row(L"Study ID", value(0x00200010));
        row(L"Study instance UID", value(0x0020000D));
        row(L"Series", std::to_wstring(count(Level::Series)));
        {
            std::wstring modalities;
            for (int c : node.children)
            {
                const std::wstring m = Widen(files[nodes[c].files[0]].modality);
                if (!m.empty() && (L" " + modalities + L" ").find(L" " + m + L" ") == std::wstring::npos)
                    modalities += (modalities.empty() ? L"" : L" ") + m;
            }
            row(L"Modalities", modalities);
        }
        break;
    case Level::Series:
        row(L"Modality", Widen(g.modality));
        row(L"Series number", Widen(g.seriesNumber));
        row(L"Description", Widen(g.seriesDescription, g.charset));
        row(L"Date", DateTimeText(g.seriesDate, g.seriesTime));
        row(L"Protocol", value(0x00181030));
        row(L"Body part", value(0x00180015));
        {
            std::wstring device = value(0x00080070);
            const std::wstring model = value(0x00081090);
            if (!model.empty()) device += (device.empty() ? L"" : L" ") + model;
            row(L"Device", device);
        }
        row(L"Series instance UID", Widen(g.series));
        row(L"Kind (guessed)", KindOf(n));
        row(L"Orientations", std::to_wstring(count(Level::Orientation)));
        row(L"Positions", std::to_wstring(count(Level::Position)));
        break;
    case Level::Orientation:
    {
        Header h;
        h.imageOrientation = g.orientationText;
        row(L"Plane", PlaneName(h));
        row(L"Image orientation", Widen(g.orientationText));
        swprintf_s(buffer, L"%u x %u", g.columns, g.rows);
        row(L"Columns x rows", buffer);
        row(L"Pixel spacing", value(0x00280030));
        row(L"Slice thickness", value(0x00180050));
        row(L"Positions", std::to_wstring(node.children.size()));
        size_t least = SIZE_MAX, most = 0;
        for (int c : node.children) { least = (std::min)(least, nodes[c].files.size()); most = (std::max)(most, nodes[c].files.size()); }
        row(L"Images per position", least == most ? std::to_wstring(least) : std::to_wstring(least) + L" to " + std::to_wstring(most));
        std::vector<double> zs;
        for (int c : node.children) if (files[nodes[c].files[0]].hasZ) zs.push_back(files[nodes[c].files[0]].z);
        if (zs.size() == node.children.size() && !zs.empty())
        {
            swprintf_s(buffer, L"%.2f to %.2f mm", zs.front(), zs.back());
            row(L"Z", buffer);
            if (zs.size() > 1)
            {
                std::vector<double> steps;
                for (size_t k = 1; k < zs.size(); k++) steps.push_back(zs[k] - zs[k - 1]);
                std::sort(steps.begin(), steps.end());
                swprintf_s(buffer, L"%.3g mm (%.3g to %.3g)", steps[steps.size() / 2], steps.front(), steps.back());
                row(L"Spacing of positions", buffer);
            }
        }
        break;
    }
    case Level::Position:
    {
        row(L"Image position", Widen(g.positionText));
        if (g.hasZ) { swprintf_s(buffer, L"%.3f mm", g.z); row(L"Z", buffer); }
        std::vector<double> t;
        for (int f : node.files) if (!files[f].trigger.empty()) t.push_back(atof(files[f].trigger.c_str()));
        if (!t.empty())
        {
            std::sort(t.begin(), t.end());
            swprintf_s(buffer, L"%.0f to %.0f ms", t.front(), t.back());
            row(L"Trigger times", buffer);
        }
        break;
    }
    case Level::Image:
        row(L"File", g.path);
        row(L"Instance number", Widen(g.instance));
        row(L"SOP instance UID", value(0x00080018));
        row(L"Object", ObjectType(Narrow(value(0x00080016))));
        row(L"Compression", Compression(Narrow(value(0x00020010))));
        {
            std::wstring planes;
            for (Uint32 group : OverlayGroupsOf(node.files[0])) { wchar_t b[8]; swprintf_s(b, L"%04X", group); planes += (planes.empty() ? L"" : L", ") + std::wstring(b); }
            row(L"Overlay planes", planes);
        }
        swprintf_s(buffer, L"%u x %u", g.columns, g.rows);
        row(L"Columns x rows", buffer);
        if (g.frame >= 0) row(L"Frame", std::to_wstring(g.frame + 1) + L" of the enhanced file");
        else if (g.frames > 1) row(L"Frames", std::to_wstring(g.frames));
        row(L"Trigger time", g.trigger.empty() ? L"" : Widen(g.trigger) + L" ms");
        row(L"Z", g.hasZ ? std::to_wstring(g.z) + L" mm" : L"");
        row(L"Acquired", g.time.size() >= 8 ? DateTimeText(g.time.substr(0, 8), g.time.substr(8)) : L"");
        break;
    default:
        break;
    }
    if (node.level != Level::Image)
    {
        row(L"Files", std::to_wstring(FilesOf(n)) + (frames > static_cast<long>(FilesOf(n)) ? L"  (" + std::to_wstring(frames) + L" frames)" : L""));
        std::wstring where = *folders.begin();
        if (where.size() > root.size() && _wcsnicmp(where.c_str(), root.c_str(), root.size()) == 0) where = L"." + where.substr(root.size());
        else if (where.size() == root.size()) where = L".";
        row(L"Folders", folders.size() == 1 ? where : std::to_wstring(folders.size()));
    }
    swprintf_s(buffer, L"%.1f MB", bytes / 1048576.0);
    row(L"Size", buffer);
    // The notes of its series (not problems: in the color of the rest)
    if (node.level >= Level::Series)
    {
        const auto& notes = nodes[SeriesOf(n)].notes;
        for (size_t k = 0; k < notes.size(); k++) row(k ? L"" : L"Notes", notes[k]);
    }
    firstProblemRow = static_cast<int>(rows.size());
    const std::vector<std::wstring> problems = ProblemsUnder(n, 40);
    for (size_t k = 0; k < problems.size(); k++) row(k ? L"" : L"\u26A0 Problems", problems[k]);

    SendMessageW(info, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(info);
    for (size_t i = 0; i < rows.size(); i++)
    {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(rows[i].first.c_str());
        const int at = ListView_InsertItem(info, &it);
        ListView_SetItemText(info, at, 1, const_cast<wchar_t*>(rows[i].second.c_str()));
    }
    SendMessageW(info, WM_SETREDRAW, TRUE, 0);
}

// The attributes all the files of a node share: those its children share (an image: its own)
const Attributes& DicomExplorer::Common(int n)
{
    GatherNode& node = nodes[n];
    if (node.hasCommon) return node.common;
    if (node.level == Level::Image) node.common = data->files[node.files[0]].attributes;
    else
    {
        for (size_t k = 0; k < node.children.size(); k++)
        {
            const Attributes& c = Common(node.children[k]);
            if (k == 0) { nodes[n].common = c; continue; }
            Attributes both;
            const Attributes& a = nodes[n].common;
            size_t i = 0, j = 0;
            while (i < a.size() && j < c.size())
            {
                if (a[i].first < c[j].first) i++;
                else if (c[j].first < a[i].first) j++;
                else
                {
                    if (a[i].second == c[j].second) both.push_back(a[i]);
                    i++;
                    j++;
                }
            }
            nodes[n].common = std::move(both);
        }
    }
    nodes[n].hasCommon = true;
    return nodes[n].common;
}

const std::pair<Uint32, Uint32>* DicomExplorer::Find(const Attributes& a, Uint32 tag)
{
    auto it = std::lower_bound(a.begin(), a.end(), tag, [](const std::pair<Uint32, Uint32>& x, Uint32 t) { return x.first < t; });
    return it != a.end() && it->first == tag ? &*it : nullptr;
}

// The value of an attribute in a node: the one of all its files, "(varies)", or nothing
std::wstring DicomExplorer::Value(int n, Uint32 tag)
{
    if (const auto* v = Find(Common(n), tag)) return Shown(tag, data->pool.Text(v->second));
    for (int f : nodes[n].files) if (Find(data->files[f].attributes, tag)) return L"(varies)";
    return L"";
}

std::wstring DicomExplorer::Shown(Uint32 tag, const std::wstring& value) const
{
    return hideIdentity && IdentifyingTag(tag) && !value.empty() ? L"(hidden)" : value;
}

// ----- the full info

bool DicomExplorer::Matches(const std::wstring& text, const std::wstring& what, const std::wregex& re, bool isRegex)
{
    if (what.empty()) return true;
    if (isRegex)
    {
        try { return std::regex_search(text, re); }
        catch (...) { return false; }
    }
    return StrStrIW(text.c_str(), what.c_str()) != nullptr;
}

// Text, or a regular expression when it is one (it has its characters and compiles)
void DicomExplorer::Compile(const std::wstring& text, std::wregex& re, bool& isRegex)
{
    isRegex = false;
    if (text.find_first_of(L"^$[]()|*+?\\{}.") == std::wstring::npos) return;
    try
    {
        re = std::wregex(text, std::regex_constants::icase | std::regex_constants::ECMAScript);
        isRegex = true;
    }
    catch (...) {}
}

// The attributes of the node and of the nodes above it, level by level: at each one those its files
// share that the files of the one above do not all share; then those that vary in the node
void DicomExplorer::FillFull(int n)
{
    SendMessageW(full, WM_SETREDRAW, FALSE, 0);
    // (the groups folded or not, as they were left: by their own arrow or by a click on their title)
    for (size_t id = 1; id < groupKind.size(); id++)
    {
        if (ListView_GetGroupState(full, static_cast<int>(id), LVGS_COLLAPSED) & LVGS_COLLAPSED) folded.insert(groupKind[id]);
        else folded.erase(groupKind[id]);
    }
    groupKind.assign(1, -1);
    groupTitle.assign(1, std::wstring());
    ListView_DeleteAllItems(full);
    ListView_RemoveAllGroups(full);
    static const wchar_t* const levels[] = { L"All the files", L"Patient", L"Study", L"Series", L"Orientation", L"Position", L"Image" };
    int groupId = 0, row = 0;
    auto put =[&](const std::wstring& header, int kind, const std::vector<Row>& rows) {
        std::vector<const Row*> keep;
        for (const Row& r : rows)
        {
            const std::wstring text = TagText(r.tag) + L" " + TagName(r.tag) + L" " + r.value;
            if (Matches(text, filterText, filterRegex, filterIsRegex)) keep.push_back(&r);
        }
        if (keep.empty()) return;
        const std::wstring plain = header + L"   (" + std::to_wstring(keep.size()) + L")";
        const std::wstring title = GroupTitle(plain, folded.count(kind) != 0);
        LVGROUP g = {};
        g.cbSize = sizeof(g);
        g.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        g.pszHeader = const_cast<wchar_t*>(title.c_str());
        g.iGroupId = ++groupId;
        g.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
        g.state = folded.count(kind) ? LVGS_COLLAPSED : 0;      // (not collapsible: its triangle folds it, not an arrow at the right)
        ListView_InsertGroup(full, -1, &g);
        groupKind.push_back(kind);
        groupTitle.push_back(plain);
        for (const Row* r : keep)
        {
            const std::wstring tag = TagText(r->tag), name = TagName(r->tag);
            LVITEMW it = {};
            it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
            it.iItem = row++;
            it.iGroupId = groupId;
            it.lParam = kind;
            it.pszText = const_cast<wchar_t*>(tag.c_str());
            const int at = ListView_InsertItem(full, &it);
            ListView_SetItemText(full, at, 1, const_cast<wchar_t*>(name.c_str()));
            ListView_SetItemText(full, at, 2, const_cast<wchar_t*>(r->value.c_str()));
        }
    };
    // The node and those above it (not the whole set when it is one patient)
    std::vector<int> chain;
    for (int k = n; k >= 0; k = nodes[k].parent)
        if (k > 0 || nodes[0].children.size() > 1) chain.push_back(k);
    const auto& pool = data->pool;
    for (size_t c = 0; c < chain.size(); c++)
    {
        const int k = chain[c];
        const Attributes& mine = Common(k);
        const Attributes* above = c + 1 < chain.size() ? &Common(chain[c + 1]) : nullptr;
        std::vector<Row> rows;
        for (const auto& a : mine)
        {
            const auto* b = above ? Find(*above, a.first) : nullptr;
            if (b && b->second == a.second) continue;
            rows.push_back({ a.first, Shown(a.first, pool.Text(a.second)) });
        }
        const GatherNode& node = nodes[k];
        std::wstring header = levels[static_cast<int>(node.level)];
        if (node.level == Level::All) header = L"Common to all the " + HowMany(k);
        else if (node.level == Level::Image) header += L":   " + node.text;
        else header += L":   " + node.text + L"   -   common to its " + HowMany(k);
        put(header, static_cast<int>(node.level), rows);
        if (c == 0 && node.files.size() > 1) put(L"Varies in the " + std::wstring(levels[static_cast<int>(node.level)]) +
                                                 L" (number of values: the first ones)", 100, Varying(k));
    }
    SendMessageW(full, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(full, nullptr, TRUE);
}

// Those that are not the same in all the files of a node: how many values, the first ones
std::vector<DicomExplorer::Row> DicomExplorer::Varying(int n)
{
    struct Spread { size_t files = 0; std::vector<Uint32> first; std::unordered_set<Uint32> ids; };
    std::map<Uint32, Spread> spread;
    const Attributes& mine = Common(n);
    for (int f : nodes[n].files)
        for (const auto& a : data->files[f].attributes)
        {
            if (Find(mine, a.first)) continue;
            Spread& s = spread[a.first];
            s.files++;
            if (s.ids.insert(a.second).second && s.first.size() < 3) s.first.push_back(a.second);
        }
    std::vector<Row> rows;
    const size_t total = nodes[n].files.size();
    for (const auto& [tag, s] : spread)
    {
        std::wstring v = std::to_wstring(s.ids.size()) + (s.ids.size() == 1 ? L" value: " : L" values: ");
        if (hideIdentity && IdentifyingTag(tag)) v += L"(hidden)";
        else
            for (size_t i = 0; i < s.first.size(); i++)
            {
                std::wstring one = data->pool.Text(s.first[i]);
                if (one.size() > 60) one = one.substr(0, 60) + L"...";
                v += (i ? L"  |  " : L"") + one;
            }
        if (s.ids.size() > s.first.size()) v += L"  |  ...";
        if (s.files < total) v += L"   (not in " + std::to_wstring(total - s.files) + L" files)";
        rows.push_back({ tag, v });
    }
    return rows;
}

// The title of a group of the full info, after the triangle that folds it (down: open; right: folded)
std::wstring DicomExplorer::GroupTitle(const std::wstring& title, bool fold)
{
    return (fold ? L"\u25B6   " : L"\u25BC   ") + title;
}

// The group of the full info whose triangle is under a point (0: none)
int DicomExplorer::TriangleAt(HWND h, POINT pt) const
{
    // (by where the titles are: the hit test of the list says which group only in lists of owner data)
    for (int id = 1; id < static_cast<int>(groupKind.size()); id++)
    {
        RECT title = {}, label = {};
        if (!ListView_GetGroupRect(h, id, LVGGR_HEADER, &title) || !PtInRect(&title, pt)) continue;
        if (!ListView_GetGroupRect(h, id, LVGGR_LABEL, &label)) return 0;
        return pt.x >= label.left - Px(10) && pt.x < label.left + Px(22) ? id : 0;
    }
    return 0;
}

// A click on the triangle of a group of the full info: folded, or open again (and so it stays)
LRESULT CALLBACK DicomExplorer::FullProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    auto self = reinterpret_cast<DicomExplorer*>(ref);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
    {
        const int id = self->TriangleAt(h, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        if (id > 0 && id < static_cast<int>(self->groupKind.size()))
        {
            ListView_SetItemState(h, -1, 0, LVIS_SELECTED);
            const bool fold = !(ListView_GetGroupState(h, id, LVGS_COLLAPSED) & LVGS_COLLAPSED);
            const std::wstring title = GroupTitle(self->groupTitle[id], fold);
            LVGROUP g = {};
            g.cbSize = sizeof(g);
            g.mask = LVGF_HEADER | LVGF_STATE;
            g.pszHeader = const_cast<wchar_t*>(title.c_str());
            g.stateMask = LVGS_COLLAPSED;
            g.state = fold ? LVGS_COLLAPSED : 0;
            ListView_SetGroupInfo(h, id, &g);
            if (fold) self->folded.insert(self->groupKind[id]);
            else self->folded.erase(self->groupKind[id]);
            return 0;
        }
        // (the rest of a title: nothing - the list would choose all the rows of the group)
        LVHITTESTINFO hit = {};
        hit.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ListView_HitTestEx(h, &hit);
        if (hit.flags & LVHT_EX_GROUP_HEADER) return 0;
    }
    if (msg == WM_LBUTTONUP)
    {
        LVHITTESTINFO hit = {};
        hit.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ListView_HitTestEx(h, &hit);
        if (hit.flags & LVHT_EX_GROUP_HEADER) return 0;     // (the list chooses the rows of a group when its title is let go)
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT)
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        if (self->TriangleAt(h, pt)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, FullProc, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

// A click on the title of the Tag column (not on its edge): folded, or open
LRESULT CALLBACK DicomExplorer::HeaderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    auto self = reinterpret_cast<DicomExplorer*>(ref);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
    {
        HDHITTESTINFO hit = {};
        hit.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (SendMessageW(h, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)) == 0 && (hit.flags & HHT_ONHEADER)) { self->TagColumn(!self->tagOpen); return 0; }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, HeaderProc, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

// ----- the plot

// The tag of a row of the full info (0: none)
Uint32 DicomExplorer::RowTag(int row) const
{
    wchar_t b[32] = {};
    ListView_GetItemText(full, row, 0, b, ARRAYSIZE(b));
    unsigned g = 0, e = 0;
    return swscanf_s(b, L"(%x,%x)", &g, &e) == 2 ? (g << 16) | e : 0;
}

void DicomExplorer::PlotRow(int row)
{
    const Uint32 tag = RowTag(row);
    if (!tag || selected <= 0) return;
    plotTag = tag;
    const bool was = plot.Visible();
    plot.Show(true);
    FillPlot(selected);
    if (!was) Layout();
}

// The values of the attribute in the files of a node, in their order
void DicomExplorer::FillPlot(int n)
{
    if (!plotTag || n <= 0 || n >= static_cast<int>(nodes.size())) return;
    plotFiles = nodes[n].files;
    std::vector<std::wstring> values, names;
    int current = -1;
    for (size_t i = 0; i < plotFiles.size(); i++)
    {
        const int f = plotFiles[i];
        const std::wstring* v = AttributeOf(f, plotTag);
        values.push_back(v ? Shown(plotTag, *v) : std::wstring());
        const Gathered& g = data->files[f];
        names.push_back(g.path.substr(g.path.find_last_of(L'\\') + 1) + (g.frame >= 0 ? L" frame " + std::to_wstring(g.frame + 1) : L""));
        if (f == currentFile) current = static_cast<int>(i);
    }
    const std::wstring name = TagName(plotTag);
    plot.Set(TagText(plotTag) + L" " + (name.empty() ? L"" : name) + L"   along " + HowMany(n), std::move(values), std::move(names), current);
}

// A click on a file of the plot: its image chosen in the tree
void DicomExplorer::PlotPicked(int i)
{
    if (i < 0 || i >= static_cast<int>(plotFiles.size())) return;
    const int image = fileNode[plotFiles[i]];
    if (HTREEITEM item = image > 0 ? ItemOf(image) : nullptr)
    {
        TreeView_SelectItem(tree, item);
        TreeView_EnsureVisible(tree, item);
    }
}

// The menu of the full info: the plot of the attribute of a row, the rows copied
void DicomExplorer::FullMenu(LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int row = -1;
    if (pt.x == -1 && pt.y == -1)
    {
        row = ListView_GetNextItem(full, -1, LVNI_FOCUSED);
        RECT r = {};
        if (row >= 0 && ListView_GetItemRect(full, row, &r, LVIR_LABEL)) { pt = { r.left, r.bottom }; ClientToScreen(full, &pt); }
        else GetCursorPos(&pt);
    }
    else
    {
        LVHITTESTINFO hit = {};
        hit.pt = pt;
        ScreenToClient(full, &hit.pt);
        row = ListView_HitTest(full, &hit);
    }
    const Uint32 tag = row >= 0 ? RowTag(row) : 0;
    HMENU menu = CreatePopupMenu();
    const std::wstring name = tag ? TagName(tag) : std::wstring();
    AppendMenuW(menu, MF_STRING | (tag && selected > 0 ? 0 : MF_GRAYED), 1,
                (L"&Plot " + (name.empty() ? (tag ? TagText(tag) : std::wstring(L"the attribute")) : name) + L" along the files").c_str());
    AppendMenuW(menu, MF_STRING, 2, L"&Copy the rows chosen");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) PlotRow(row);
    else if (cmd == 2) CopyRows(full, true);
}

void DicomExplorer::TagColumn(bool open)
{
    tagOpen = open;
    WriteSetting(L"TagColumn", open ? 1 : 0);
    std::wstring title = open ? L"\u25BE Tag" : L"\u25B8";
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = &title[0];
    c.cx = Px(open ? 95 : 22);
    ListView_SetColumn(full, 0, &c);
    InvalidateRect(full, nullptr, TRUE);
}
