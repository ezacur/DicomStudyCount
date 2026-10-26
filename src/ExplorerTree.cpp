// The DICOM explorer: its tree (DICOMgather.m), the unused tree, a node chosen, find, the context menu
#include "Shell.h"

// ----- the tree of DICOMgather.m

int DicomExplorer::Add(Level level, int parent, std::vector<int> files)
{
    GatherNode n;
    n.level = level;
    n.parent = parent;
    n.files = std::move(files);
    nodes.push_back(std::move(n));
    const int i = static_cast<int>(nodes.size()) - 1;
    if (parent >= 0) nodes[parent].children.push_back(i);
    return i;
}

void DicomExplorer::Build()
{
    nodes.clear();
    const std::vector<Gathered>& files = data->files;
    fileState.assign(files.size(), FileState());
    // Derived or original (the first value of ImageType), and the frame of reference: volumes apart
    for (size_t f = 0; f < files.size(); f++)
    {
        if (const auto* a = Find(files[f].attributes, 0x00080008))
        {
            const std::wstring& t = data->pool.Text(a->second);
            fileState[f].derived = t.size() >= 7 && _wcsnicmp(t.c_str(), L"DERIVED", 7) == 0;
        }
        if (const auto* a = Find(files[f].attributes, 0x00200052)) fileState[f].frame = a->second + 1;
    }
    // The images in their order first (instance number, trigger time, time, name): the groups below
    // a series come in the order of their first image
    std::vector<int> all(files.size());
    std::iota(all.begin(), all.end(), 0);
    std::sort(all.begin(), all.end(), [&](int a, int b) {
        const Gathered &x = files[a], &y = files[b];
        const long i = atol(x.instance.c_str()), j = atol(y.instance.c_str());
        if (i != j) return i < j;
        const double s = atof(x.trigger.c_str()), t = atof(y.trigger.c_str());
        if (s != t) return s < t;
        if (x.time != y.time) return x.time < y.time;
        const int byName = StrCmpLogicalW(x.path.c_str(), y.path.c_str());
        return byName ? byName < 0 : x.frame < y.frame;     // (the frames of a file in their order)
    });
    for (size_t i = 0; i < all.size(); i++) fileState[all[i]].rank = static_cast<int>(i);
    const int top = Add(Level::All, -1, all);
    // The patients by their PatientID; the people that share one (9999, none), apart by who they are
    struct Patient { std::vector<int> files; std::wstring person; int sharing = 1; };
    std::vector<Patient> patients;
    for (auto& pg : GroupBy(all, [&](int f) { return files[f].patient; }))
    {
        // Each file with the first person it can be (the most complete ones first: they make the people)
        std::vector<Person> who(pg.size());
        std::vector<size_t> order(pg.size());
        std::unordered_map<std::string, size_t> seen;         // (name and birth date as read: their person once)
        for (size_t i = 0; i < pg.size(); i++)
        {
            const Gathered& g = files[pg[i]];
            const std::string raw = g.patientName + '\x01' + g.birthDate + '\x01' + g.charset;
            auto it = seen.find(raw);
            who[i] = it != seen.end() ? who[it->second] : PersonOf(g.patientName, g.charset, g.birthDate);
            if (it == seen.end()) seen.emplace(raw, i);
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return (!who[a].birth.empty()) + (!who[a].name.empty()) > (!who[b].birth.empty()) + (!who[b].name.empty());
        });
        std::vector<Person> people;
        std::vector<std::vector<int>> theirs;
        std::vector<int> nobody;                // neither a name nor a birth date: cannot be told
        for (size_t i : order)
        {
            if (who[i].Empty()) { nobody.push_back(pg[i]); continue; }
            size_t k = 0;
            while (k < people.size() && !SamePerson(people[k], who[i])) k++;
            if (k == people.size()) { people.push_back(who[i]); theirs.emplace_back(); }
            theirs[k].push_back(pg[i]);
        }
        if (people.size() <= 1)
        {
            patients.push_back({ pg, L"", 1 });
            continue;
        }
        if (!nobody.empty()) { people.push_back(Person()); theirs.push_back(nobody); }
        for (size_t k = 0; k < theirs.size(); k++)
        {
            std::sort(theirs[k].begin(), theirs[k].end(), [&](int a, int b) { return fileState[a].rank < fileState[b].rank; });
            std::string key = people[k].birth + "|";
            for (wchar_t c : people[k].name) key += static_cast<char>(c < 128 ? c : '?');
            patients.push_back({ theirs[k], people[k].Empty() ? L"no name or birth date" : Hash6(key), static_cast<int>(theirs.size()) });
        }
    }
    std::sort(patients.begin(), patients.end(), [&](const Patient& a, const Patient& b) {
        const std::string &x = files[a.files[0]].patient, &y = files[b.files[0]].patient;
        return x != y ? x < y : a.person < b.person;
    });
    for (auto& pt : patients)
    {
        const std::vector<int>& pg = pt.files;
        const int p = Add(Level::Patient, top, pg);
        nodes[p].person = pt.person;
        nodes[p].sharing = pt.sharing;
        auto studies = GroupBy(pg, [&](int f) { return files[f].study; });
        std::sort(studies.begin(), studies.end(), [&](const auto& a, const auto& b) {
            const Gathered &x = files[a[0]], &y = files[b[0]];
            if (x.studyDate + x.studyTime != y.studyDate + y.studyTime) return x.studyDate + x.studyTime < y.studyDate + y.studyTime;
            return x.study < y.study;
        });
        for (auto& sg : studies)
        {
            const int s = Add(Level::Study, p, sg);
            auto series = GroupBy(sg, [&](int f) { return files[f].series; });
            std::sort(series.begin(), series.end(), [&](const auto& a, const auto& b) {
                const Gathered &x = files[a[0]], &y = files[b[0]];
                const long i = atol(x.seriesNumber.c_str()), j = atol(y.seriesNumber.c_str());
                if (i != j) return i < j;
                if (x.seriesDate + x.seriesTime != y.seriesDate + y.seriesTime) return x.seriesDate + x.seriesTime < y.seriesDate + y.seriesTime;
                return x.series < y.series;
            });
            for (auto& rg : series) AddSeries(s, rg);
        }
    }
    // The files of every node in the order of the tree: those of its children, one after the other
    for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; i--)
    {
        if (nodes[i].children.empty()) continue;
        std::vector<int> f;
        for (int c : nodes[i].children) f.insert(f.end(), nodes[c].files.begin(), nodes[c].files.end());
        nodes[i].files = std::move(f);
    }
    Check();
    for (size_t i = 0; i < nodes.size(); i++) nodes[i].text = Label(static_cast<int>(i));
}

// ----- the checks: what may be wrong in the series

// The files of a node (the frames of an enhanced file are images of their own, but one file)
size_t DicomExplorer::FilesOf(int n) const
{
    size_t k = 0;
    for (int f : nodes[n].files) if (data->files[f].frame <= 0) k++;
    return k;
}

// "24 files", or "120 images (1 file)" when frames of enhanced files are among them
std::wstring DicomExplorer::HowMany(int n) const
{
    const size_t files = FilesOf(n), images = nodes[n].files.size();
    const std::wstring f = std::to_wstring(files) + (files == 1 ? L" file" : L" files");
    return files == images ? f : std::to_wstring(images) + L" images (" + f + L")";
}

// The line of a node in the tree, and what the checks found in it
std::wstring DicomExplorer::Label(int i) const
{
    std::wstring s = Plain(i);
    for (int m = 0; m < Marks; m++)
        if (i < static_cast<int>(markCount[m].size()) && markCount[m][i])
            s += nodes[i].level == Level::Image ? L"   " + std::wstring(MarkKinds[m].image)
                                                : L"   (" + std::to_wstring(markCount[m][i]) + L" " + MarkKinds[m].counted + L")";
    const auto& flags = nodes[i].flags;
    for (size_t k = 0; k < flags.size(); k++) s += (k ? L", " : L"   \u26A0 ") + flags[k];
    return s;
}

// What kind of series a node is of (its series), guessed as the column of the files does: its first file,
// and the number of images of its positions when the header does not say how many phases
std::wstring DicomExplorer::KindOf(int n) const
{
    while (n > 0 && nodes[n].level > Level::Series) n = nodes[n].parent;
    if (n <= 0 || nodes[n].level != Level::Series || nodes[n].files.empty()) return std::wstring();
    const int f = nodes[n].files[0];
    const Gathered& g = data->files[f];
    auto get = [&](Uint32 tag) { const std::wstring* s = AttributeOf(f, tag); return s ? Narrow(*s) : std::string(); };
    Header h;
    h.sopClass = get(0x00080016);
    h.modality = g.modality;
    h.imageType = get(0x00080008);
    h.seriesDescription = g.seriesDescription;
    h.protocolName = get(0x00181030);
    h.sequenceName = get(0x00180024);
    h.scanningSequence = get(0x00180020);
    h.inversionTime = get(0x00180082);
    h.cardiacImages = get(0x00181090);
    h.imageOrientation = g.orientationText;
    if (h.cardiacImages.empty() && !g.trigger.empty())
    {
        size_t most = 0;
        std::function<void(int)> walk = [&](int x) {
            if (nodes[x].level == Level::Position) { most = (std::max)(most, nodes[x].files.size()); return; }
            for (int c : nodes[x].children) walk(c);
        };
        walk(n);
        if (most > 1) h.cardiacImages = std::to_string(most);
    }
    return SeriesKind(h, PlaneName(h));
}

std::wstring DicomExplorer::Plain(int i) const
{
    const GatherNode& n = nodes[i];
    if (n.files.empty()) return L"";
    const Gathered& g = data->files[n.files[0]];
    const size_t count = n.files.size();
    auto files = [&](size_t) { return HowMany(i); };
    wchar_t buffer[200];
    std::wstring s;
    auto add = [&](const std::wstring& part) { if (!part.empty()) s += (s.empty() ? L"" : L"   ") + part; };
    switch (n.level)
    {
    case Level::All:
        return L"All";
    case Level::Patient:
        add((g.patient.empty() ? L"Patient (no ID)" : L"Patient " + Widen(g.patient, g.charset)) +
            (n.person.empty() ? L"" : L" \x00B7 [" + n.person + L"]"));
        add(L"(" + std::to_wstring(n.children.size()) + (n.children.size() == 1 ? L" study, " : L" studies, ") + files(count) + L")");
        return s;
    case Level::Study:
        add(DateTimeText(g.studyDate, g.studyTime));
        add(Widen(g.studyDescription, g.charset));
        if (!g.study.empty()) add(L"[" + Hash6(g.study) + L"]");
        if (s.empty()) s = L"Study";
        add(L"(" + std::to_wstring(n.children.size()) + L" series, " + files(count) + L")");
        return s;
    case Level::Series:
    {
        add(Widen(g.modality));
        add(g.seriesNumber.empty() ? L"Series" : L"Series " + Widen(g.seriesNumber));
        add(Widen(g.seriesDescription, g.charset));
        // One orientation: its plane and size here
        if (n.children.size() == 1) add(Size(n.children[0]));
        else add(L"(" + std::to_wstring(n.children.size()) + L" orientations)");
        add(L"(" + files(count) + L")");
        const std::wstring kind = KindOf(i);
        if (!kind.empty()) add(L"\u00AB" + kind + L"\u00BB");     // (guessed)
        return s;
    }
    case Level::Orientation:
    {
        add(Size(i));
        if (!fileState.empty() && fileState[n.files[0]].derived) add(L"derived");
        // (a series in more than one frame of reference: which one)
        bool others = false;
        for (int o : nodes[n.parent].children)
            if (fileState[nodes[o].files[0]].frame != fileState[n.files[0]].frame) others = true;
        if (others && fileState[n.files[0]].frame) add(L"[FoR " + Hash6(Narrow(data->pool.Text(fileState[n.files[0]].frame - 1))) + L"]");
        add(L"(" + files(count) + L")");
        return s;
    }
    case Level::Position:
        if (g.hasZ) { swprintf_s(buffer, L"Z %.2f mm", g.z); add(buffer); }
        else if (g.placed) add(L"Position " + Widen(g.positionText));
        else add(L"No position");
        if (count > 1) add(L"(" + std::to_wstring(count) + L" images)");
        return s;
    case Level::Image:
    {
        add(g.instance.empty() ? L"Image" : L"Image " + Widen(g.instance));
        if (!g.trigger.empty()) { swprintf_s(buffer, L"t %.1f ms", atof(g.trigger.c_str())); add(buffer); }
        const GatherNode& position = nodes[n.parent];
        if (g.hasZ && (compact || position.files.size() == 1)) { swprintf_s(buffer, L"Z %.2f", g.z); add(buffer); }
        if (g.frame >= 0) add(L"frame " + std::to_wstring(g.frame + 1));     // (of an enhanced file)
        else if (g.frames > 1) add(std::to_wstring(g.frames) + L" frames");
        add(g.path.substr(g.path.find_last_of(L'\\') + 1));     // (its folder: in the info of the node)
        return s;
    }
    }
    return s;
}

// An orientation: its plane, rows x columns x positions (x images in each, when they are as many)
std::wstring DicomExplorer::Size(int o) const
{
    const GatherNode& n = nodes[o];
    const Gathered& g = data->files[n.files[0]];
    Header h;
    h.imageOrientation = g.orientationText;
    std::wstring plane = PlaneName(h);
    wchar_t buffer[120];
    swprintf_s(buffer, L"%u x %u x %zu", g.columns, g.rows, n.children.size());
    std::wstring s = (plane.empty() ? L"" : plane + L"  ") + buffer;
    const size_t per = nodes[n.children[0]].files.size();
    if (std::all_of(n.children.begin(), n.children.end(), [&](int c) { return nodes[c].files.size() == per; }) && per > 1)
        s += L" x " + std::to_wstring(per);
    return s;
}

// Compact: an orientation alone in its series, and positions that hold one image each (or that are
// alone in their orientation), are not in the tree: what is under them is
bool DicomExplorer::Skipped(int c) const
{
    const GatherNode& n = nodes[c];
    if (n.parent < 0) return false;
    const GatherNode& p = nodes[n.parent];
    if (n.level == Level::Orientation) return p.children.size() == 1;
    if (n.level == Level::Position)
        return p.children.size() == 1 ||
               std::all_of(p.children.begin(), p.children.end(), [&](int k) { return nodes[k].files.size() == 1; });
    return false;
}

void DicomExplorer::ComputeShown()
{
    for (auto& n : nodes) { n.shown.clear(); n.shownParent = -1; n.item = nullptr; n.filled = false; }
    std::function<void(int, std::vector<int>&)> collect = [&](int c, std::vector<int>& out) {
        if (compact && Skipped(c)) for (int k : nodes[c].children) collect(k, out);
        else out.push_back(c);
    };
    for (size_t i = 0; i < nodes.size(); i++)
    {
        if (compact && Skipped(static_cast<int>(i))) continue;
        std::vector<int> shown;
        for (int c : nodes[i].children) collect(c, shown);
        for (int c : shown) nodes[c].shownParent = static_cast<int>(i);
        nodes[i].shown = std::move(shown);
    }
    for (size_t i = 0; i < nodes.size(); i++)
        if (nodes[i].level == Level::Image) nodes[i].text = Label(static_cast<int>(i));     // (Z or not)
}

// A node in the tree (some of its files in use), or in the unused one (some of them put aside)
bool DicomExplorer::InTree(int n, bool unused) const
{
    if (usedCount.empty()) return !unused;
    return unused ? usedCount[n] < nodes[n].files.size() : usedCount[n] > 0;
}

bool DicomExplorer::HasShown(int n, bool unused) const
{
    for (int c : nodes[n].shown) if (InTree(c, unused)) return true;
    return false;
}

// The shown children of a node with files put aside into the unused tree
void DicomExplorer::InsertUnused(int n)
{
    if (n < 0 || unusedFilled.empty() || unusedFilled[n]) return;
    HTREEITEM under = n == 0 ? TVI_ROOT : unusedItem[n];
    if (!under) return;
    unusedFilled[n] = 1;
    for (int c : nodes[n].shown)
    {
        if (!InTree(c, true)) continue;
        TVINSERTSTRUCTW is = {};
        is.hParent = under;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        is.item.pszText = const_cast<wchar_t*>(nodes[c].text.c_str());
        is.item.lParam = c;
        is.item.cChildren = HasShown(c, true) ? 1 : 0;
        unusedItem[c] = TreeView_InsertItem(unusedTree, &is);
    }
}

HTREEITEM DicomExplorer::UnusedItemOf(int n)
{
    if (n <= 0 || n >= static_cast<int>(unusedItem.size())) return nullptr;
    if (unusedItem[n]) return unusedItem[n];
    const int p = nodes[n].shownParent;
    if (p < 0 || (p > 0 && !UnusedItemOf(p))) return nullptr;
    InsertUnused(p);
    return unusedItem[n];
}

// A tree again (after nodes went from one to the other), as it was: what was open, chosen and at its top
void DicomExplorer::Refill(bool unused)
{
    HWND t = unused ? unusedTree : tree;
    auto itemOf = [&](size_t i) { return unused ? (i < unusedItem.size() ? unusedItem[i] : nullptr) : nodes[i].item; };
    auto nodeOf = [&](HTREEITEM it) {
        TVITEMW tv = { TVIF_PARAM };
        tv.hItem = it;
        return it && TreeView_GetItem(t, &tv) ? static_cast<int>(tv.lParam) : -1;
    };
    std::vector<int> open;
    for (size_t i = 1; i < nodes.size(); i++)
        if (HTREEITEM it = itemOf(i))
            if (TreeView_GetItemState(t, it, TVIS_EXPANDED) & TVIS_EXPANDED) open.push_back(static_cast<int>(i));
    const int chosen = nodeOf(TreeView_GetSelection(t)), first = nodeOf(TreeView_GetFirstVisible(t));
    SendMessageW(t, WM_SETREDRAW, FALSE, 0);
    following = true;
    TreeView_DeleteAllItems(t);
    if (unused) { std::fill(unusedItem.begin(), unusedItem.end(), nullptr); std::fill(unusedFilled.begin(), unusedFilled.end(), 0); }
    else for (auto& n : nodes) { n.item = nullptr; n.filled = false; }
    if (unused) InsertUnused(0); else Insert(0);
    for (int n : open) if (HTREEITEM it = itemOf(n)) TreeView_Expand(t, it, TVE_EXPAND);     // (parents first)
    if (chosen > 0) if (HTREEITEM it = itemOf(chosen)) TreeView_SelectItem(t, it);
    following = false;
    if (first > 0) if (HTREEITEM it = itemOf(first)) TreeView_SelectSetFirstVisible(t, it);
    SendMessageW(t, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(t, nullptr, TRUE);
    foundStale = true;
}

// A node to the unused tree (its files put aside: not in the navigator), or back to the tree
void DicomExplorer::MoveUnused(int n, bool aside)
{
    if (!ready || n <= 0 || n >= static_cast<int>(nodes.size())) return;
    HidePopup();
    for (int f : nodes[n].files) fileState[f].unused = aside;
    for (size_t i = 0; i < nodes.size(); i++)
    {
        size_t k = 0;
        for (int f : nodes[i].files) if (!fileState[f].unused) k++;
        usedCount[i] = k;
    }
    Refill(false);
    Refill(true);
    // (where it went, in sight)
    HTREEITEM it = aside ? UnusedItemOf(n) : ItemOf(n);
    if (it) TreeView_EnsureVisible(aside ? unusedTree : tree, it);
    UnusedLabel();
    if (selected > 0) Navigate(selected);
}

void DicomExplorer::UnusedLabel()
{
    if (!unusedLabel) return;
    size_t k = 0;
    for (const FileState& s : fileState) k += s.unused;
    SetWindowTextW(unusedLabel, k ? (L"Unused   (" + std::to_wstring(k) + (k == 1 ? L" image)" : L" images)")).c_str()
                                  : L"Unused   (middle click a node, or right-click it: Move to unused)");
}

// The shown children of a node into the tree (when it is opened: a series can have thousands)
void DicomExplorer::Insert(int n)
{
    if (n < 0 || nodes[n].filled) return;
    nodes[n].filled = true;
    HTREEITEM under = n == 0 ? TVI_ROOT : nodes[n].item;
    if (!under) return;
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    for (int c : nodes[n].shown)
    {
        if (!InTree(c, false)) continue;    // (put aside: in the unused tree)
        TVINSERTSTRUCTW is = {};
        is.hParent = under;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_STATE;
        is.item.pszText = const_cast<wchar_t*>(nodes[c].text.c_str());
        is.item.lParam = c;
        is.item.state = INDEXTOSTATEIMAGEMASK(StateOf(c));
        is.item.stateMask = TVIS_STATEIMAGEMASK;
        is.item.cChildren = HasShown(c, false) ? 1 : 0;
        nodes[c].item = TreeView_InsertItem(tree, &is);
    }
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
}

// The tree item of a node, its parents opened if they were not
HTREEITEM DicomExplorer::ItemOf(int n)
{
    if (n <= 0 || n >= static_cast<int>(nodes.size())) return nullptr;
    if (nodes[n].item) return nodes[n].item;
    const int p = nodes[n].shownParent;
    if (p < 0) return nullptr;
    if (p > 0 && !ItemOf(p)) return nullptr;
    Insert(p);
    return nodes[n].item;
}

// Patients and studies open (the series of a few studies in sight)
void DicomExplorer::FillTree()
{
    foundStale = true;
    following = true;
    TreeView_DeleteAllItems(tree);
    following = false;
    if (nodes.empty()) return;
    Insert(0);
    size_t studies = 0;
    for (int p : nodes[0].shown) studies += nodes[p].shown.size();
    for (int p : nodes[0].shown)
    {
        TreeView_Expand(tree, nodes[p].item, TVE_EXPAND);
        if (studies <= 12) for (int s : nodes[p].shown) TreeView_Expand(tree, nodes[s].item, TVE_EXPAND);
    }
}

// Compact or not: the tree again, as open as it was (the levels that appear, open when the node above
// them is: what was seen is seen still), with the same node chosen (or the one shown that holds it) and
// the same one at its top
void DicomExplorer::Recompact()
{
    if (!ready) return;
    int keep = selected;
    std::vector<char> wasOpen(nodes.size(), 0), wasIn(nodes.size(), 0);
    openMemory.resize(nodes.size(), -1);
    for (size_t i = 1; i < nodes.size(); i++)
    {
        wasIn[i] = nodes[i].shownParent >= 0;
        if (nodes[i].item && (TreeView_GetItemState(tree, nodes[i].item, TVIS_EXPANDED) & TVIS_EXPANDED)) wasOpen[i] = 1;
        if (nodes[i].item) openMemory[i] = wasOpen[i];
    }
    int first = -1;
    if (HTREEITEM top = TreeView_GetFirstVisible(tree))
    {
        TVITEMW tv = { TVIF_PARAM };
        tv.hItem = top;
        if (TreeView_GetItem(tree, &tv)) first = static_cast<int>(tv.lParam);
    }
    ComputeShown();
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    following = true;
    TreeView_DeleteAllItems(tree);
    following = false;
    Insert(0);
    // (parents before their children: their items are there by then)
    for (size_t i = 1; i < nodes.size(); i++)
    {
        HTREEITEM it = nodes[i].item;
        if (!it || nodes[i].shown.empty()) continue;
        const int p = nodes[i].shownParent;
        const bool parentOpen = p <= 0 || (nodes[p].item && (TreeView_GetItemState(tree, nodes[p].item, TVIS_EXPANDED) & TVIS_EXPANDED));
        // (as it was; one that comes back, as it was then; a new one, as the node above it)
        const bool open = wasIn[i] ? wasOpen[i] != 0 : openMemory[i] >= 0 ? openMemory[i] != 0 : parentOpen;
        if (open) TreeView_Expand(tree, it, TVE_EXPAND);
    }
    Refill(true);
    while (keep > 0 && nodes[keep].shownParent < 0) keep = nodes[keep].parent;
    while (first > 0 && nodes[first].shownParent < 0) first = nodes[first].parent;
    following = true;
    if (HTREEITEM item = ItemOf(keep)) TreeView_SelectItem(tree, item);
    following = false;
    if (first > 0 && nodes[first].item) TreeView_SelectSetFirstVisible(tree, nodes[first].item);
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree, nullptr, TRUE);
    foundStale = true;
}

// ----- a node chosen

void DicomExplorer::Choose(int n)
{
    if (n <= 0 || n >= static_cast<int>(nodes.size())) return;
    selected = n;
    FillInfo(n);
    FillFull(n);
    Navigate(n);
    if (plot.Visible()) FillPlot(n);
    if (!preview) return;
    // An image: the slider goes through those of the node above it in the tree; another node: its own
    int under = n, index = 0;
    if (nodes[n].level == Level::Image && nodes[n].shownParent > 0)
    {
        under = nodes[n].shownParent;
        const auto& f = nodes[under].files;
        index = static_cast<int>(std::find(f.begin(), f.end(), nodes[n].files[0]) - f.begin());
    }
    if (under == sliderNode && nodes[n].level == Level::Image)
    {
        preview->ShowIndex(index);
        return;
    }
    sliderNode = under;
    std::vector<std::wstring> paths;
    std::vector<int> frames;                    // (of enhanced files: which frame each image is)
    // Where each one is: in time, its position (whose images are its times); in space, its plane - or,
    // under a study, its series (the jumps go from series to series)
    std::vector<int> space, time;
    const bool bySeries = nodes[under].level <= Level::Study;
    for (int f : nodes[under].files)
    {
        paths.push_back(data->files[f].path);
        frames.push_back(data->files[f].frame);
        int position = fileState[f].node, s = -1;
        while (position > 0 && nodes[position].level != Level::Position) position = nodes[position].parent;
        if (position <= 0) position = fileState[f].node;
        for (int a = fileState[f].node; a > 0; a = nodes[a].parent) if (nodes[a].level == Level::Series) { s = a; break; }
        time.push_back(position);
        space.push_back(bySeries && s > 0 ? s : position);
    }
    const int delay = bySeries ? 700 : 100;
    const wchar_t* places = bySeries ? L"Series" : L"Plane";
    // One plane (a cine of one slice): its images themselves, the cine playing - over them the reference
    // lines can be drawn
    currentFile = -1;
    Segments();
    if (nodes[n].level != Level::Image)
    {
        MakePlanes();
        const int p0 = fileState[nodes[under].files[0]].plane;
        bool one = p0 >= 0;
        for (int f : nodes[under].files) if (fileState[f].plane != p0) { one = false; break; }
        if (one)
        {
            preview->ShowFiles(paths, 0, frames, space, time, delay, places);
            if (nodes[under].files.size() > 1) preview->StartCine();
            return;
        }
    }
    // Not an image: what the floating thumbnail shows (the middle slice, the mosaic, the cine...),
    // larger, until the slider goes to its files
    if (nodes[n].level != Level::Image)
    {
        preview->ShowFiles(paths, -1, frames, space, time, delay, places);
        if (posters.count(n)) ShowPoster(n);
        else AskThumb(n, true);
        return;
    }
    preview->ShowFiles(paths, index, frames, space, time, delay, places);
}

void DicomExplorer::ShowPoster(int n)
{
    auto it = posters.find(n);
    if (!preview || it == posters.end() || n != sliderNode) return;
    if (preview->Stepping()) return;            // (the files are being gone through already: they stay)
    const Thumb& t = *it->second;
    if (t.frames.empty()) { preview->ShowIndex(0); return; }      // (no thumbnail: the first file)
    std::vector<PreviewHandler::PosterFrame> frames;
    for (const Picture& p : t.frames) frames.push_back({ p.width, p.height, p.px });
    std::wstring caption = nodes[n].text;
    if (!t.caption.empty()) caption += L"   -   " + t.caption;
    preview->ShowPoster(std::move(frames), t.delay, caption);
}

// ----- find in the tree

std::vector<int> DicomExplorer::Order() const
{
    std::vector<int> order;
    std::function<void(int)> walk = [&](int i) { for (int c : nodes[i].shown) if (InTree(c, false)) { order.push_back(c); walk(c); } };
    if (!nodes.empty()) walk(0);
    return order;
}

bool DicomExplorer::Hit(int n) const { return !findText.empty() && Matches(nodes[n].text, findText, findRegex, findIsRegex); }

bool DicomExplorer::Found(int n, bool unused)
{
    if (foundStale) MarkFound();
    const auto& f = findMarks[unused ? 1 : 0];
    return n >= 0 && n < static_cast<int>(f.size()) && f[n];
}

void DicomExplorer::MarkFound()
{
    foundStale = false;
    for (auto& f : findMarks) f.assign(nodes.size(), 0);
    if (!ready || findText.empty()) return;
    if (findHits.size() != nodes.size())     // (another folder read)
    {
        findHits.assign(nodes.size(), 0);
        for (size_t n = 1; n < nodes.size(); n++) findHits[n] = Hit(static_cast<int>(n));
    }
    for (int t = 0; t < 2; t++)
    {
        HWND h = t ? unusedTree : tree;
        auto itemOf = [&](int i) -> HTREEITEM { return t ? (i < static_cast<int>(unusedItem.size()) ? unusedItem[i] : nullptr) : nodes[i].item; };
        auto open = [&](int i) { HTREEITEM it = itemOf(i); return it && (TreeView_GetItemState(h, it, TVIS_EXPANDED) & TVIS_EXPANDED); };
        std::vector<int> chain;
        for (size_t n = 1; n < nodes.size(); n++)
        {
            if (!findHits[n] || !InTree(static_cast<int>(n), t == 1)) continue;
            int x = static_cast<int>(n);
            while (x > 0 && nodes[x].shownParent < 0) x = nodes[x].parent;     // (left out by Compact: the one above)
            chain.clear();
            for (int a = x; a > 0; a = nodes[a].shownParent) chain.push_back(a);
            std::reverse(chain.begin(), chain.end());
            // From the top down, while the branch is open
            int at = -1;
            for (size_t k = 0; k < chain.size(); k++)
            {
                if (!itemOf(chain[k])) break;
                at = chain[k];
                if (!open(chain[k])) break;
            }
            if (at > 0) findMarks[t][at] = 1;
        }
    }
}

void DicomExplorer::FoundAgain()
{
    foundStale = true;
    if (findText.empty()) return;
    InvalidateRect(tree, nullptr, FALSE);
    InvalidateRect(unusedTree, nullptr, FALSE);
}

void DicomExplorer::FindChanged()
{
    findText = TextOf(findBox);
    Compile(findText, findRegex, findIsRegex);
    findHits.assign(nodes.size(), 0);
    if (!findText.empty()) for (size_t n = 1; n < nodes.size(); n++) findHits[n] = Hit(static_cast<int>(n));
    foundStale = true;
    InvalidateRect(tree, nullptr, TRUE);
    InvalidateRect(unusedTree, nullptr, TRUE);
    if (!ready) return;
    if (findText.empty())
    {
        SetStatus(summary);
        return;
    }
    size_t hits = 0;
    for (int n : Order()) if (Hit(n)) hits++;
    SetStatus(std::to_wstring(hits) + (hits == 1 ? L" node matches \"" : L" nodes match \"") + findText + L"\"   (Enter / F3: next, Shift+F3: previous)");
}

void DicomExplorer::FindNext(bool back)
{
    if (!ready) return;
    if (TextOf(findBox) != findText) FindChanged();
    if (findText.empty()) return;
    const std::vector<int> order = Order();
    if (order.empty()) return;
    const int n = static_cast<int>(order.size());
    int at = static_cast<int>(std::find(order.begin(), order.end(), selected) - order.begin());
    if (at >= n) at = back ? n : -1;
    for (int k = 1; k <= n; k++)
    {
        const int i = ((at + (back ? -k : k)) % n + n) % n;
        if (!Hit(order[i])) continue;
        if (HTREEITEM item = ItemOf(order[i]))
        {
            TreeView_SelectItem(tree, item);
            TreeView_EnsureVisible(tree, item);
        }
        return;
    }
    SetStatus(L"No node matches \"" + findText + L"\"");
}

// ----- context menu, copying

std::vector<std::wstring> DicomExplorer::PathsOf(int n) const
{
    std::vector<std::wstring> paths;
    for (int f : nodes[n].files)
        if (data->files[f].frame <= 0) paths.push_back(data->files[f].path);   // (an enhanced file once)
    return paths;
}

void DicomExplorer::CopyPaths(int n)
{
    std::wstring text;
    for (const auto& p : PathsOf(n)) text += p + L"\r\n";
    CopyText(wnd, text);
}

// The rows of a list (the selected ones, or all), a tab between the columns; the groups as titles
void DicomExplorer::CopyRows(HWND list, bool selection)
{
    const int columns = Header_GetItemCount(ListView_GetHeader(list));
    std::wstring text;
    int group = -1;
    wchar_t buffer[4096];
    for (int i = 0; i < ListView_GetItemCount(list); i++)
    {
        if (selection && !(ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED)) continue;
        LVITEMW it = {};
        it.mask = LVIF_GROUPID;
        it.iItem = i;
        ListView_GetItem(list, &it);
        if (list == full && it.iGroupId != group)
        {
            group = it.iGroupId;
            LVGROUP g = {};
            g.cbSize = sizeof(g);
            g.mask = LVGF_HEADER;
            g.pszHeader = buffer;
            g.cchHeader = ARRAYSIZE(buffer);
            if (ListView_GetGroupInfo(list, group, &g)) text += (text.empty() ? L"# " : L"\r\n# ") + std::wstring(buffer) + L"\r\n";
        }
        for (int c = 0; c < columns; c++)
        {
            ListView_GetItemText(list, i, c, buffer, ARRAYSIZE(buffer));
            text += (c ? L"\t" : L"") + std::wstring(buffer);
        }
        text += L"\r\n";
    }
    CopyText(wnd, text);
}

void DicomExplorer::TreeMenu(LPARAM lp, bool unused)
{
    if (!ready) return;
    HWND t = unused ? unusedTree : tree;       // (the one clicked)
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    HTREEITEM item = nullptr;
    if (pt.x == -1 && pt.y == -1)
    {
        item = TreeView_GetSelection(t);
        RECT r = {};
        if (item && TreeView_GetItemRect(t, item, &r, TRUE)) { pt = { r.left, r.bottom }; ClientToScreen(t, &pt); }
    }
    else
    {
        TVHITTESTINFO hit = {};
        hit.pt = pt;
        ScreenToClient(t, &hit.pt);
        item = TreeView_HitTest(t, &hit);
        if (item) TreeView_SelectItem(t, item);
    }
    if (!item) return;
    TVITEMW tv = { TVIF_PARAM };
    tv.hItem = item;
    if (!TreeView_GetItem(t, &tv)) return;
    const int n = static_cast<int>(tv.lParam);
    HMENU menu = CreatePopupMenu();
    if (unused)
        AppendMenuW(menu, MF_STRING, 10, L"Move back to the &tree");
    else
    {
        AppendMenuW(menu, MF_STRING, 1, nodes[n].files.size() == 1 ? L"Show in Explorer" : L"Show the files in Explorer");
        AppendMenuW(menu, MF_STRING, 2, nodes[n].files.size() == 1 ? L"Copy the path" : L"Copy the paths of the files");
        AppendMenuW(menu, MF_STRING, 3, L"Copy the full info");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (nodes[n].shown.empty() ? MF_GRAYED : 0), 4, L"Expand all under it");
        AppendMenuW(menu, MF_STRING | (nodes[n].shown.empty() ? MF_GRAYED : 0), 5, L"Collapse all under it");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const UINT busy = job ? MF_GRAYED : 0;
        AppendMenuW(menu, MF_STRING | busy, 6, L"E&xport the images (PNG, animated GIF)...");
        AppendMenuW(menu, MF_STRING | busy, 7, L"Export to &NIfTI...");
        AppendMenuW(menu, MF_STRING | busy, 8, L"&Report (HTML)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 9, L"Move to &unused");
    }
    HidePopup();
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, nullptr);
    DestroyMenu(menu);
    switch (cmd)
    {
    case 1: ShowInExplorer(PathsOf(n)); break;
    case 2: CopyPaths(n); break;
    case 3: CopyRows(full, false); break;
    case 4: ExpandAll(n, true); break;
    case 5: ExpandAll(n, false); break;
    case 6: Export(n, false); break;
    case 7: Export(n, true); break;
    case 8: Report(n); break;
    case 9: MoveUnused(n, true); break;
    case 10: MoveUnused(n, false); break;
    }
}
