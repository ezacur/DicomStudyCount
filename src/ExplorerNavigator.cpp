// The DICOM explorer: the navigator of a node, the check boxes, the reference lines, the segmentations marked
#include "Shell.h"

// ----- the navigator

// The plane of each image that has a position, orientation, size and pixel spacing; the images in the
// same plane (of a position: its times, its copies) share it
void DicomExplorer::MakePlanes()
{
    if (planesMade || !data) return;
    planesMade = true;
    const std::vector<Gathered>& files = data->files;
    for (FileState& s : fileState) s.plane = -1;
    std::vector<std::pair<std::array<int, 12>, int>> keyed;    // (corners in 1/100 mm: the same plane, the same key)
    std::vector<NavPlane> each;
    for (int f = 0; f < static_cast<int>(files.size()); f++)
    {
        const Gathered& g = files[f];
        if (!g.oriented || !g.placed || !g.rows || !g.columns) continue;
        const std::wstring* s = AttributeOf(f, 0x00280030);
        double dy = 0, dx = 0;
        if (!s || swscanf_s(s->c_str(), L"%lf\\%lf", &dy, &dx) != 2 || dy <= 0 || dx <= 0) continue;
        // From the middle of the first pixel along its row (orientation 1..3) and its column (4..6)
        NavPlane q;
        const double u[2] = { -0.5, g.columns - 0.5 }, v[2] = { -0.5, g.rows - 0.5 };
        std::array<int, 12> key;
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 3; k++)
            {
                const double p = g.position[k] + u[c & 1] * dx * g.orientation[k] + v[c >> 1] * dy * g.orientation[3 + k];
                q.p[c][k] = static_cast<float>(p);
                key[c * 3 + k] = static_cast<int>(std::lround(p * 100));
            }
        keyed.push_back({ key, static_cast<int>(each.size()) });
        each.push_back(q);
        fileState[f].plane = static_cast<int>(each.size()) - 1;
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<int> to(each.size());
    navPlanes.clear();
    for (size_t i = 0; i < keyed.size(); i++)
    {
        if (i == 0 || keyed[i].first != keyed[i - 1].first) navPlanes.push_back(each[keyed[i].second]);
        to[keyed[i].second] = static_cast<int>(navPlanes.size()) - 1;
    }
    for (FileState& s : fileState) if (s.plane >= 0) s.plane = to[s.plane];
}

// The navigator on a node: the planes of everything whose check box is on, of all the patients (each
// study has coordinates of its own, so those of others are where their scanner put them: turn off what
// is not wanted), its own in red; on its plane its image, if it has one plane
void DicomExplorer::Navigate(int n)
{
    if (!navigator.Visible() || !ready || n <= 0 || n >= static_cast<int>(nodes.size())) return;
    MakePlanes();
    const int context = 0;
    int patient = n;
    while (patient > 0 && nodes[patient].level > Level::Patient) patient = nodes[patient].parent;
    std::vector<int> mine, around;
    std::vector<char> seen(navPlanes.size(), 0);
    for (int f : nodes[n].files)
        if (fileState[f].on && !fileState[f].unused && fileState[f].plane >= 0 && !seen[fileState[f].plane]) { seen[fileState[f].plane] = 1; mine.push_back(fileState[f].plane); }
    for (int f : nodes[context].files)
        if (fileState[f].on && !fileState[f].unused && fileState[f].plane >= 0 && !seen[fileState[f].plane]) { seen[fileState[f].plane] = 2; around.push_back(fileState[f].plane); }
    // Too many to draw (a patient, everything): some of them, evenly
    const size_t most = 40000;
    auto thin = [](std::vector<int>& v, size_t keep) {
        if (v.size() <= keep) return;
        std::vector<int> some;
        for (size_t i = 0; i < keep; i++) some.push_back(v[i * v.size() / keep]);
        v.swap(some);
    };
    const size_t red = mine.size(), total = mine.size() + around.size();
    thin(mine, most);
    thin(around, most > mine.size() ? most - mine.size() : 0);
    std::vector<NavPlane> scene;
    std::vector<char> hot;
    Light(-1);
    marked = -1;
    navScene.clear();
    for (int p : around) { scene.push_back(navPlanes[p]); hot.push_back(0); navScene.push_back(p); }
    for (int p : mine) { scene.push_back(navPlanes[p]); hot.push_back(1); navScene.push_back(p); }
    navSceneOf.assign(navPlanes.size(), -1);
    for (size_t k = 0; k < navScene.size(); k++) navSceneOf[navScene[k]] = static_cast<int>(k);
    // One plane: its image (the first one, the chosen one if an image)
    Picture picture;
    int on = -1;
    if (mine.size() == 1)
    {
        const int f = nodes[n].files[0];
        Slice s;
        if (DecodeSlice(data->files[f].path, (std::max)(0, data->files[f].frame), false, s).empty() && s.width > 0 && s.height > 0)
        {
            const RECT box = { 0, 0, s.width, s.height };
            double lo = 0, hi = 255;
            if (s.mono) Percentiles({ s.values.data() }, s.width, box, 0.02, 0.98, lo, hi);
            const int side = 512;
            const double k = (std::max)(s.width, s.height) > side ? static_cast<double>(side) / (std::max)(s.width, s.height) : 1;
            picture = Shrink(s, box, lo, hi, (std::max)(1, static_cast<int>(s.width * k)), (std::max)(1, static_cast<int>(s.height * k)));
            on = static_cast<int>(scene.size()) - 1;
        }
    }
    std::wstring more;
    if (!total)
    {
        // (none drawn: none has a position, or their check boxes in the tree are off)
        for (int f : nodes[context].files)
            if (fileState[f].plane >= 0) { more = L"Its images are turned off (the check boxes of the tree)"; break; }
    }
    else
    {
        wchar_t b[200];
        if (total == red) swprintf_s(b, L"%zu plane%s", red, red == 1 ? L"" : L"s");
        else swprintf_s(b, L"%zu plane%s in red, of %zu turned on", red, red == 1 ? L"" : L"s", total);
        more = b;
        if (total > most) more += L" (" + std::to_wstring(most) + L" of them drawn)";
    }
    if (usedCount[n] == 0) more = L"Put aside in the unused tree: not drawn here" + (total ? L"  (" + std::to_wstring(total) + L" planes turned on)" : std::wstring());
    navigator.Set(std::move(scene), std::move(hot), on, std::move(picture), nodes[n].text, more, patient != navPatient);
    navigated = context;
    navPatient = patient;
}

// The check box of a node: 1 off, 2 on, 3 some of it on (its files drawn in the navigator, or not)
// Its image in the tree: the same box in the colors of the marks of the images under it (MarkKinds): 3 more
// for each combination of them (the bits of the marks it has)
int DicomExplorer::StateOf(int n) const
{
    int bits = 0;
    for (int m = 0; m < Marks; m++)
        if (n >= 0 && n < static_cast<int>(markCount[m].size()) && markCount[m][n] > 0) bits |= 1 << m;
    return CheckOf(n) + 3 * bits;
}

// The images of the check boxes of the tree: off, on and partly on; plain, and in the colors of each
// combination of the marks
void DicomExplorer::CheckImages()
{
    HIMAGELIST before = TreeView_GetImageList(tree, TVSIL_STATE);
    int size = 0, ignore = 0;
    if (!before || !ImageList_GetIconSize(before, &size, &ignore) || size <= 0) size = Px(16);
    const int images = 1 + 3 * (1 << Marks);
    HIMAGELIST list = ImageList_Create(size, size, ILC_COLOR32, images, 0);
    if (!list) return;
    HTHEME theme = OpenThemeData(tree, L"BUTTON");
    HDC screen = GetDC(tree);
    const COLORREF back = GetSysColor(COLOR_WINDOW);
    for (int k = 0; k < images; k++)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) continue;
        HDC dc = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(dc, bitmap);
        RECT all = { 0, 0, size, size };
        HBRUSH brush = CreateSolidBrush(back);
        FillRect(dc, &all, brush);
        DeleteObject(brush);
        RECT box = all;
        if (k > 0)
        {
            const int kind = (k - 1) % 3;           // (off, on, partly)
            SIZE part = { size - 2, size - 2 };
            if (theme) GetThemePartSize(theme, dc, BP_CHECKBOX, CBS_UNCHECKEDNORMAL, nullptr, TS_TRUE, &part);
            box = { (size - part.cx) / 2, (size - part.cy) / 2, (size - part.cx) / 2 + part.cx, (size - part.cy) / 2 + part.cy };
            if (theme)
                DrawThemeBackground(theme, dc, BP_CHECKBOX, kind == 0 ? CBS_UNCHECKEDNORMAL : kind == 1 ? CBS_CHECKEDNORMAL : CBS_MIXEDNORMAL, &box, nullptr);
            else
                DrawFrameControl(dc, &box, DFC_BUTTON, DFCS_BUTTONCHECK | (kind == 1 ? DFCS_CHECKED : 0));
            // The marks: the box in their colors (its blue, and an empty one light inside), a band of it
            // for each one, from the left
            std::vector<int> marks;
            for (int m = 0; m < Marks; m++) if (((k - 1) / 3) & (1 << m)) marks.push_back(m);
            if (!marks.empty())
            {
                GdiFlush();
                DWORD* px = static_cast<DWORD*>(bits);
                const int width = box.right - box.left;
                for (int y = box.top + 1; y < box.bottom - 1; y++)
                    for (int x = box.left + 1; x < box.right - 1; x++)
                    {
                        const MarkKind& mark = MarkKinds[marks[(std::min)(marks.size() - 1, static_cast<size_t>((x - box.left) * static_cast<int>(marks.size()) / width))]];
                        DWORD& p = px[static_cast<size_t>(y) * size + x];
                        const int c[3] = { static_cast<int>((p >> 16) & 0xFF), static_cast<int>((p >> 8) & 0xFF), static_cast<int>(p & 0xFF) };
                        if (c[2] > c[0] + 30 && c[2] > c[1]) p = (c[mark.from[0]] << 16) | (c[mark.from[1]] << 8) | c[mark.from[2]];
                        else if (c[0] > 200 && c[1] > 200 && c[2] > 200 && kind == 0) p = mark.light;
                    }
            }
        }
        // (opaque: the image list draws its alpha)
        GdiFlush();
        DWORD* px = static_cast<DWORD*>(bits);
        for (int i = 0; i < size * size; i++) px[i] |= 0xFF000000;
        SelectObject(dc, old);
        DeleteDC(dc);
        ImageList_Add(list, bitmap, nullptr);
        DeleteObject(bitmap);
    }
    ReleaseDC(tree, screen);
    if (theme) CloseThemeData(theme);
    TreeView_SetImageList(tree, list, TVSIL_STATE);
    if (before && before != list) ImageList_Destroy(before);
}

int DicomExplorer::CheckOf(int n) const
{
    if (n < 0 || n >= static_cast<int>(onCount.size())) return 2;
    return onCount[n] == 0 ? 1 : onCount[n] == nodes[n].files.size() ? 2 : 3;
}

// Its check box clicked: all of it on (off if it all was), and so everything under it
void DicomExplorer::Toggle(int n)
{
    if (!ready || n <= 0 || n >= static_cast<int>(nodes.size())) return;
    const bool on = onCount[n] < nodes[n].files.size();
    for (int f : nodes[n].files) fileState[f].on = on;
    const long long delta = static_cast<long long>(on ? nodes[n].files.size() : 0) - static_cast<long long>(onCount[n]);
    std::vector<int> down = { n };
    while (!down.empty())
    {
        const int d = down.back();
        down.pop_back();
        onCount[d] = on ? nodes[d].files.size() : 0;
        for (int c : nodes[d].children) down.push_back(c);
    }
    for (int a = nodes[n].parent; a >= 0; a = nodes[a].parent) onCount[a] = static_cast<size_t>(static_cast<long long>(onCount[a]) + delta);
    for (size_t i = 1; i < nodes.size(); i++)
        if (nodes[i].item) TreeView_SetItemState(tree, nodes[i].item, INDEXTOSTATEIMAGEMASK(StateOf(static_cast<int>(i))), TVIS_STATEIMAGEMASK);
    if (selected > 0) Navigate(selected);
}

bool DicomExplorer::GeometryOf(int f, Geometry& g) const
{
    if (f < 0 || f >= static_cast<int>(data->files.size())) return false;
    const Gathered& x = data->files[f];
    if (!x.oriented || !x.placed || !x.rows || !x.columns) return false;
    const std::wstring* s = AttributeOf(f, 0x00280030);
    double dy = 0, dx = 0;
    if (!s || swscanf_s(s->c_str(), L"%lf\\%lf", &dy, &dx) != 2 || dy <= 0 || dx <= 0) return false;
    for (int k = 0; k < 3; k++) { g.origin[k] = x.position[k]; g.row[k] = x.orientation[k]; g.column[k] = x.orientation[3 + k]; }
    g.dx = dx;
    g.dy = dy;
    g.columns = static_cast<int>(x.columns);
    g.rows = static_cast<int>(x.rows);
    g.thickness = 0;
    if (const std::wstring* t = AttributeOf(f, 0x00180050)) g.thickness = _wtof(t->c_str());
    return true;
}

// The file the image panel shows now (the slider moved): the reference lines on it again
void DicomExplorer::ShownFile(int i)
{
    const int before = currentFile;
    currentFile = sliderNode >= 0 && i >= 0 && i < static_cast<int>(nodes[sliderNode].files.size()) ? nodes[sliderNode].files[i] : -1;
    if (currentFile != before && marked > 0) Guides(marked);
    if (currentFile != before) Segments();
    if (plot.Visible())
    {
        const auto it = std::find(plotFiles.begin(), plotFiles.end(), currentFile);
        plot.SetCurrent(it == plotFiles.end() ? -1 : static_cast<int>(it - plotFiles.begin()));
    }
}

// The reference lines: where the planes of a node cut the plane of the image shown, over it and in the
// navigator (-1: none)
void DicomExplorer::Guides(int n)
{
    std::vector<std::array<double, 4>> flat;
    std::vector<std::array<float, 6>> lines;
    Geometry a;
    if (refLines && n > 0 && n < static_cast<int>(nodes.size()) && GeometryOf(currentFile, a))
    {
        MakePlanes();
        const double normal[3] = { a.row[1] * a.column[2] - a.row[2] * a.column[1], a.row[2] * a.column[0] - a.row[0] * a.column[2],
                                   a.row[0] * a.column[1] - a.row[1] * a.column[0] };
        const int mine = currentFile < static_cast<int>(fileState.size()) ? fileState[currentFile].plane : -1;
        std::vector<char> seen(navPlanes.size(), 0);
        for (int f : nodes[n].files)
        {
            const int p = f < static_cast<int>(fileState.size()) ? fileState[f].plane : -1;
            if (p < 0 || p == mine || seen[p] || lines.size() >= 400) continue;
            seen[p] = 1;
            // Where its edges cross the plane of the image (from its corners: 0 1 / 2 3)
            const auto& q = navPlanes[p].p;
            double d[4];
            for (int c = 0; c < 4; c++)
            {
                d[c] = 0;
                for (int k = 0; k < 3; k++) d[c] += (q[c][k] - a.origin[k]) * normal[k];
            }
            std::vector<std::array<double, 3>> cut;
            static const int edges[4][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 } };
            for (const auto& e : edges)
            {
                const double u = d[e[0]], v = d[e[1]];
                if ((u < 0) == (v < 0) || u == v) continue;
                const double t = u / (u - v);
                std::array<double, 3> x;
                for (int k = 0; k < 3; k++) x[k] = q[e[0]][k] + t * (q[e[1]][k] - q[e[0]][k]);
                cut.push_back(x);
            }
            if (cut.size() < 2) continue;
            // In the pixels of the image, cut to it
            auto pixel = [&](const std::array<double, 3>& x, double& c, double& r) {
                double u = 0, v = 0;
                for (int k = 0; k < 3; k++) { u += (x[k] - a.origin[k]) * a.row[k]; v += (x[k] - a.origin[k]) * a.column[k]; }
                c = u / a.dx;
                r = v / a.dy;
            };
            double c0, r0, c1, r1;
            pixel(cut[0], c0, r0);
            pixel(cut[1], c1, r1);
            double t0 = 0, t1 = 1;
            const double lo[2] = { -0.5, -0.5 }, hi[2] = { a.columns - 0.5, a.rows - 0.5 }, p0[2] = { c0, r0 }, dp[2] = { c1 - c0, r1 - r0 };
            bool keep = true;
            for (int k = 0; k < 2 && keep; k++)
            {
                if (std::fabs(dp[k]) < 1e-12) { if (p0[k] < lo[k] || p0[k] > hi[k]) keep = false; continue; }
                double ta = (lo[k] - p0[k]) / dp[k], tb = (hi[k] - p0[k]) / dp[k];
                if (ta > tb) std::swap(ta, tb);
                t0 = (std::max)(t0, ta);
                t1 = (std::min)(t1, tb);
                if (t0 > t1) keep = false;
            }
            if (!keep) continue;
            flat.push_back({ c0 + t0 * dp[0], r0 + t0 * dp[1], c0 + t1 * dp[0], r0 + t1 * dp[1] });
            std::array<float, 6> line;
            for (int k = 0; k < 3; k++)
            {
                line[k] = static_cast<float>(a.origin[k] + (c0 + t0 * dp[0]) * a.dx * a.row[k] + (r0 + t0 * dp[1]) * a.dy * a.column[k]);
                line[3 + k] = static_cast<float>(a.origin[k] + (c0 + t1 * dp[0]) * a.dx * a.row[k] + (r0 + t1 * dp[1]) * a.dy * a.column[k]);
            }
            lines.push_back(line);
        }
    }
    if (preview) preview->SetGuides(std::move(flat));
    navigator.SetGuides(std::move(lines));
}

// The images with a segmentation in their plane (a frame of a SEG with something in it, or a contour of a
// structure set; their segmentations read now, only when the folder has any) or with overlay planes (60xx)
// of their own, marked in the tree
void DicomExplorer::MarkSegmented()
{
    for (Segmentation& o : segmentations) if (!o.loaded) LoadSegmentation(o);
    std::vector<char> withSegments(data->files.size(), 0);
    const std::wstring segClass = Widen(SegSopClass);
    for (size_t f = 0; f < data->files.size(); f++)
    {
        if (!OverlayGroupsOf(static_cast<int>(f)).empty()) { withSegments[f] = 1; continue; }
        Geometry a;
        if (segmentations.empty() || !GeometryOf(static_cast<int>(f), a)) continue;
        const std::wstring* own = AttributeOf(static_cast<int>(f), 0x00080016);
        if (own && *own == segClass) continue;
        for (const Segmentation& o : segmentations)
            if (o.study == data->files[f].study && SegmentsIn(o, a)) { withSegments[f] = 1; break; }
    }
    SetMark(Segmented, withSegments);
}

// A mark on the images that have it (each file: 1 if it has it), and on the nodes above them how many:
// counted, and their lines in the tree again
void DicomExplorer::SetMark(TreeMark m, const std::vector<char>& with)
{
    std::vector<size_t>& count = markCount[m];
    count.assign(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); i++) for (int f : nodes[i].files) count[i] += with[f];
    for (size_t i = 0; i < nodes.size(); i++) if (count[i]) nodes[i].text = Label(static_cast<int>(i));
}

// The segmentations of the study of the image shown, over it (the masks of the SEG frames in its plane, the
// contours of the structure sets in its plane; its overlay planes, by the preview) and in the navigator (all
// the contours)
void DicomExplorer::Segments()
{
    std::vector<DWORD> argb;
    std::vector<SegOutline> flat;
    std::vector<Navigator::Line3> lines;
    Geometry a;
    if (preview) preview->SetSegmentsShown(showSegments);
    if (showSegments && !segmentations.empty() && GeometryOf(currentFile, a))
    {
        const Gathered& shown = data->files[currentFile];
        const std::wstring* own = AttributeOf(currentFile, 0x00080016);
        const bool itself = own && (*own == Widen(SegSopClass));     // (a SEG shown: not over itself)
        std::vector<const Segmentation*> over;
        for (Segmentation& o : segmentations)
        {
            if (o.study != shown.study) continue;
            if (!o.loaded) LoadSegmentation(o);
            if (o.failed || (itself && !o.rt)) continue;
            over.push_back(&o);
            for (const Contour& c3 : o.contours)
            {
                Navigator::Line3 l;
                l.color = c3.color;
                l.closed = c3.closed;
                for (const auto& p : c3.points) l.points.push_back({ static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]) });
                if (lines.size() < 5000) lines.push_back(std::move(l));
            }
        }
        SegmentsOver(over, a, argb, flat);
    }
    if (preview) preview->SetSegmentation(argb.empty() ? 0 : a.columns, argb.empty() ? 0 : a.rows, argb, std::move(flat));
    navigator.SetContours(std::move(lines));
}

// The mouse over the image: the point of the patient there, in the navigator
void DicomExplorer::Pointed(bool on, double column, double row)
{
    Geometry a;
    if (!on || !GeometryOf(currentFile, a)) { navigator.SetPoint(false); return; }
    double p[3];
    for (int k = 0; k < 3; k++) p[k] = a.origin[k] + column * a.dx * a.row[k] + row * a.dy * a.column[k];
    navigator.SetPoint(true, p);
}

// The mouse over the navigator: where its line of sight crosses the plane of the image shown, a small
// crosshair there on the image (and the point in the navigator)
void DicomExplorer::Aimed(bool on, const double* from, const double* toward)
{
    Geometry a;
    if (!preview) return;
    if (!on || !GeometryOf(currentFile, a)) { preview->SetCrosshair(false); navigator.SetPoint(false); return; }
    const double normal[3] = { a.row[1] * a.column[2] - a.row[2] * a.column[1], a.row[2] * a.column[0] - a.row[0] * a.column[2],
                               a.row[0] * a.column[1] - a.row[1] * a.column[0] };
    double along = 0, gap = 0;
    for (int k = 0; k < 3; k++) { along += toward[k] * normal[k]; gap += (a.origin[k] - from[k]) * normal[k]; }
    if (std::fabs(along) < 0.05) { preview->SetCrosshair(false); navigator.SetPoint(false); return; }     // (seen edge on)
    double p[3], u = 0, v = 0;
    for (int k = 0; k < 3; k++) p[k] = from[k] + gap / along * toward[k];
    for (int k = 0; k < 3; k++) { u += (p[k] - a.origin[k]) * a.row[k]; v += (p[k] - a.origin[k]) * a.column[k]; }
    const double column = u / a.dx, row = v / a.dy;
    if (column < -0.5 || row < -0.5 || column > a.columns - 0.5 || row > a.rows - 0.5)
    {
        preview->SetCrosshair(false);
        navigator.SetPoint(false);
        return;
    }
    preview->SetCrosshair(true, column, row);
    navigator.SetPoint(true, p);
}

// The mouse over a node of a tree (or off them: no tree): its planes framed in black in the navigator,
// whatever the floating thumbnail does
void DicomExplorer::Mark(HWND t, POINT pt)
{
    int n = -1;
    if (t && ready)
    {
        if (!markTracking)
        {
            TRACKMOUSEEVENT e = { sizeof(e), TME_LEAVE, t, 0 };
            markTracking = TrackMouseEvent(&e) != FALSE;
        }
        TVHITTESTINFO hit = {};
        hit.pt = pt;
        HTREEITEM item = TreeView_HitTest(t, &hit);
        TVITEMW tv = { TVIF_PARAM };
        tv.hItem = item;
        if (item && (hit.flags & TVHT_ONITEM) && TreeView_GetItem(t, &tv)) n = static_cast<int>(tv.lParam);
    }
    if (n == marked) return;
    marked = n;
    Guides(n);
    std::vector<int> which;
    if (n > 0 && n < static_cast<int>(nodes.size()) && navigator.Visible() && !navScene.empty())
    {
        std::vector<char> in(navPlanes.size(), 0);
        for (int f : nodes[n].files)
            if (fileState[f].on && !fileState[f].unused && fileState[f].plane >= 0) in[fileState[f].plane] = 1;
        for (size_t i = 0; i < navScene.size(); i++) if (in[navScene[i]]) which.push_back(static_cast<int>(i));
    }
    navigator.SetMarks(std::move(which));
}

// A double click on a plane of the navigator: its first image (by instance number) chosen in the tree
void DicomExplorer::Pick(int i)
{
    if (i < 0 || i >= static_cast<int>(navScene.size()) || navigated < 0) return;
    const int p = navScene[i];
    int best = -1;
    long bestNumber = 0;
    for (int f : nodes[navigated].files)
    {
        if (fileState[f].plane != p || !fileState[f].on || fileState[f].unused) continue;
        const long number = atol(data->files[f].instance.c_str());
        if (best < 0 || number < bestNumber || (number == bestNumber && fileState[f].rank < fileState[best].rank)) { best = f; bestNumber = number; }
    }
    if (best < 0) return;
    if (HTREEITEM item = ItemOf(fileState[best].node))
    {
        TreeView_SelectItem(tree, item);
        TreeView_EnsureVisible(tree, item);
        SetFocus(tree);
    }
}

// The mouse over a plane of the navigator (-1: off them): the images of that plane marked in the tree,
// or the nearest node above them there (when their branch is closed)
void DicomExplorer::Light(int i)
{
    if (lit.empty() && i < 0) return;
    lit.clear();
    if (i >= 0 && i < static_cast<int>(navScene.size()) && navigated >= 0)
    {
        const int p = navScene[i];
        size_t images = 0;
        for (int f : nodes[navigated].files)
        {
            if (fileState[f].plane != p || !fileState[f].on || fileState[f].unused) continue;
            images++;
            int x = fileState[f].node;
            while (x > 0 && !nodes[x].item) x = nodes[x].shownParent >= 0 ? nodes[x].shownParent : nodes[x].parent;
            if (x > 0 && std::find(lit.begin(), lit.end(), x) == lit.end()) lit.push_back(x);
        }
        // What they are, in the navigator
        std::wstring text;
        if (!lit.empty())
        {
            text = nodes[lit[0]].text;
            if (lit.size() > 1) text += L"   (and " + std::to_wstring(lit.size() - 1) + L" more)";
            if (images > 1) text += L"   -   " + std::to_wstring(images) + L" images in this plane";
        }
        navigator.SetHoverText(text);
    }
    InvalidateRect(tree, nullptr, FALSE);
}

// The cine or the jumps on an image: it, on its plane in the navigator
void DicomExplorer::Live(int i)
{
    if (!preview || !preview->Running() || !navigator.Visible() || sliderNode < 0 || i < 0 ||
        i >= static_cast<int>(nodes[sliderNode].files.size()) || navSceneOf.empty())
        return;
    const int f = nodes[sliderNode].files[i];
    const int p = f < static_cast<int>(fileState.size()) ? fileState[f].plane : -1;
    const int at = p >= 0 && p < static_cast<int>(navSceneOf.size()) ? navSceneOf[p] : -1;
    int w = 0, h = 0;
    const std::vector<DWORD>* px = at >= 0 ? preview->Pixels(w, h) : nullptr;
    navigator.SetLive(at, w, h, px);
}

// The slider (or the cine) moved: an image chosen in the tree follows it
void DicomExplorer::Followed(int i)
{
    if (sliderNode < 0 || selected < 0 || i < 0 || i >= static_cast<int>(nodes[sliderNode].files.size())) return;
    if (nodes[selected].level != Level::Image || nodes[selected].shownParent != sliderNode) return;
    const int image = fileState[nodes[sliderNode].files[i]].node;
    if (image == selected || image < 0 || !nodes[image].item) return;
    following = true;
    TreeView_SelectItem(tree, nodes[image].item);
    following = false;
    selected = image;
    SetTimer(wnd, TimerFollow, 250, nullptr);   // its info when it stops for a moment
}
