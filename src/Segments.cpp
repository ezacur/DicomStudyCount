// The segmentations over the images: SEG and RT Structure Sets read, those near a file, and what of them is in
// the plane of an image
#include "Shell.h"

// A color of CIELab as DICOM writes it (L 0..65535 for 0..100, a and b 0..65535 for -128..127) in sRGB
static COLORREF FromCieLab(Uint16 l16, Uint16 a16, Uint16 b16)
{
    const double L = l16 * 100.0 / 65535, a = a16 * 255.0 / 65535 - 128, b = b16 * 255.0 / 65535 - 128;
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto inv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double X = 0.95047 * inv(fx), Y = inv(fy), Z = 1.08883 * inv(fz);
    double rgb[3] = { 3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z, 0.0557 * X - 0.2040 * Y + 1.0570 * Z };
    int out[3];
    for (int k = 0; k < 3; k++)
    {
        double v = rgb[k] <= 0.0031308 ? 12.92 * rgb[k] : 1.055 * std::pow((std::max)(0.0, rgb[k]), 1 / 2.4) - 0.055;
        out[k] = static_cast<int>(std::lround((std::max)(0.0, (std::min)(1.0, v)) * 255));
    }
    return RGB(out[0], out[1], out[2]);
}

// The value of an attribute in the first item of a sequence of an item (or of the shared groups)
static std::string InGroup(DcmItem* group, const DcmTagKey& sequence, const DcmTagKey& key)
{
    DcmSequenceOfItems* s = nullptr;
    OFString v;
    if (group && group->findAndGetSequence(sequence, s).good() && s && s->card()) s->getItem(0)->findAndGetOFStringArray(key, v);
    return std::string(v.c_str(), v.length());
}

static void NormalOf(const double* row, const double* column, double* n)
{
    n[0] = row[1] * column[2] - row[2] * column[1];
    n[1] = row[2] * column[0] - row[0] * column[2];
    n[2] = row[0] * column[1] - row[1] * column[0];
}

bool PlaneFrom(const double* orientation, const double* position, double dy, double dx, int columns, int rows,
               double thickness, ImagePlane& p)
{
    if (columns <= 0 || rows <= 0 || dx <= 0 || dy <= 0) return false;
    for (int k = 0; k < 3; k++) { p.origin[k] = position[k]; p.row[k] = orientation[k]; p.column[k] = orientation[3 + k]; }
    p.dy = dy;
    p.dx = dx;
    p.columns = columns;
    p.rows = rows;
    p.thickness = thickness > 0 ? thickness : 0;
    return true;
}

// The same, from their values as DICOM writes them
static bool PlaneFrom(const std::string& orientation, const std::string& position, const std::string& spacing,
                      int columns, int rows, const std::string& thickness, ImagePlane& p)
{
    double iop[6], ipp[3], ps[2], t = 0;
    if (!Numbers(orientation, iop, 6) || !Numbers(position, ipp, 3) || !Numbers(spacing, ps, 2)) return false;
    return PlaneFrom(iop, ipp, ps[0], ps[1], columns, rows, Numbers(thickness, &t, 1) ? t : 0, p);
}

// The plane of a frame of an enhanced file: from its functional groups (its own, or else the shared ones);
// with top, from the attributes of the file when it has them
static bool FramePlane(DcmItem* top, DcmItem* item, DcmItem* shared, int columns, int rows, ImagePlane& p)
{
    auto value = [&](const DcmTagKey& sequence, const DcmTagKey& key) {
        OFString v;
        if (top && top->findAndGetOFStringArray(key, v).good() && !v.empty()) return std::string(v.c_str(), v.length());
        std::string s = InGroup(item, sequence, key);
        return s.empty() ? InGroup(shared, sequence, key) : s;
    };
    return PlaneFrom(value(DCM_PlaneOrientationSequence, DCM_ImageOrientationPatient),
                     value(DCM_PlanePositionSequence, DCM_ImagePositionPatient),
                     value(DCM_PixelMeasuresSequence, DCM_PixelSpacing), columns, rows,
                     value(DCM_PixelMeasuresSequence, DCM_SliceThickness), p);
}

// Two planes the same: parallel, and nearer than the tolerance along their normal
static bool SamePlane(const ImagePlane& a, const ImagePlane& b, double tolerance)
{
    double na[3], nb[3], dot = 0, gap = 0;
    NormalOf(a.row, a.column, na);
    NormalOf(b.row, b.column, nb);
    for (int k = 0; k < 3; k++) { dot += na[k] * nb[k]; gap += (a.origin[k] - b.origin[k]) * nb[k]; }
    return std::fabs(dot) >= 0.999 && std::fabs(gap) <= tolerance;
}

void LoadSegmentation(Segmentation& o)
{
    o.loaded = true;
    InitDcmtk();
    DcmFileFormat ff;
    if (ff.loadFile(OFFilename(o.path.c_str(), OFTrue)).bad()) { o.failed = true; return; }
    DcmDataset* ds = ff.getDataset();
    if (o.rt)
    {
        std::map<long, COLORREF> colors;
        DcmSequenceOfItems* rois = nullptr;
        if (ds->findAndGetSequence(DcmTagKey(0x3006, 0x0039), rois).bad() || !rois) { o.failed = true; return; }
        for (unsigned long r = 0; r < rois->card(); r++)
        {
            DcmItem* roi = rois->getItem(r);
            COLORREF color = Palette[(5 + r) % PaletteSize];
            OFString rgb;
            double c[3];
            if (roi->findAndGetOFStringArray(DcmTagKey(0x3006, 0x002A), rgb).good() && Numbers(std::string(rgb.c_str()), c, 3))
                color = RGB(static_cast<int>(c[0]), static_cast<int>(c[1]), static_cast<int>(c[2]));
            DcmSequenceOfItems* contours = nullptr;
            if (roi->findAndGetSequence(DcmTagKey(0x3006, 0x0040), contours).bad() || !contours) continue;
            for (unsigned long k = 0; k < contours->card(); k++)
            {
                DcmItem* item = contours->getItem(k);
                OFString type;
                item->findAndGetOFString(DcmTagKey(0x3006, 0x0042), type);
                DcmElement* data = nullptr;
                if (item->findAndGetElement(DcmTagKey(0x3006, 0x0050), data).bad() || !data) continue;
                Contour c3;
                c3.color = color;
                c3.closed = type != "OPEN_PLANAR" && type != "OPEN_NONPLANAR";
                const unsigned long n = data->getVM();
                for (unsigned long i = 0; i + 2 < n; i += 3)
                {
                    Float64 x = 0, y = 0, z = 0;
                    if (data->getFloat64(x, i).bad() || data->getFloat64(y, i + 1).bad() || data->getFloat64(z, i + 2).bad()) break;
                    c3.points.push_back({ x, y, z });
                }
                if (!c3.points.empty()) o.contours.push_back(std::move(c3));
            }
        }
        return;
    }
    // A SEG: its segments' colors, and each frame with its plane, its segment and its mask
    DcmSequenceOfItems* segments = nullptr;
    if (ds->findAndGetSequence(DcmTagKey(0x0062, 0x0002), segments).good() && segments)
        for (unsigned long s = 0; s < segments->card(); s++)
        {
            DcmItem* item = segments->getItem(s);
            Uint16 number = 0;
            item->findAndGetUint16(DcmTagKey(0x0062, 0x0004), number);
            COLORREF color = Palette[(5 + number) % PaletteSize];
            Uint16 lab[3];
            if (item->findAndGetUint16(DcmTagKey(0x0062, 0x000D), lab[0], 0).good() &&
                item->findAndGetUint16(DcmTagKey(0x0062, 0x000D), lab[1], 1).good() &&
                item->findAndGetUint16(DcmTagKey(0x0062, 0x000D), lab[2], 2).good())
                color = FromCieLab(lab[0], lab[1], lab[2]);
            o.colors[number] = color;
        }
    Uint16 rows = 0, columns = 0, allocated = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    Sint32 count = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, count).bad() || count < 1) count = 1;
    DcmSequenceOfItems* perFrame = nullptr, *sharedSeq = nullptr;
    ds->findAndGetSequence(DCM_PerFrameFunctionalGroupsSequence, perFrame);
    DcmItem* shared = ds->findAndGetSequence(DCM_SharedFunctionalGroupsSequence, sharedSeq).good() && sharedSeq && sharedSeq->card() ? sharedSeq->getItem(0) : nullptr;
    const size_t n = static_cast<size_t>(rows) * columns;
    if (!n) { o.failed = true; return; }
    // The pixels as they are, when they are not compressed (1 bit, or 8: fractional)
    const E_TransferSyntax xfer = ds->getOriginalXfer();
    const Uint8* bytes = nullptr;
    unsigned long length = 0;
    if (!DcmXfer(xfer).isEncapsulated())
    {
        DcmElement* e = nullptr;
        Uint8* b = nullptr;
        Uint16* w = nullptr;
        if (ds->findAndGetElement(DCM_PixelData, e).good() && e)
        {
            if (e->getUint8Array(b).good() && b) bytes = b;
            else if (e->getUint16Array(w).good() && w) bytes = reinterpret_cast<const Uint8*>(w);
            length = e->getLength();
        }
    }
    std::vector<double> along;
    for (Sint32 k = 0; k < count; k++)
    {
        DcmItem* item = perFrame && static_cast<Sint32>(perFrame->card()) > k ? perFrame->getItem(static_cast<unsigned long>(k)) : nullptr;
        SegFrame f;
        if (!FramePlane(nullptr, item, shared, columns, rows, f.plane)) continue;
        std::string number = InGroup(item, DcmTagKey(0x0062, 0x000A), DcmTagKey(0x0062, 0x000B));
        if (number.empty()) number = InGroup(shared, DcmTagKey(0x0062, 0x000A), DcmTagKey(0x0062, 0x000B));
        f.segment = number.empty() ? 1 : atoi(number.c_str());
        f.mask.assign(n, 0);
        const size_t first = static_cast<size_t>(k) * n;
        if (bytes && allocated == 1 && (first + n + 7) / 8 <= length)
            for (size_t i = 0; i < n; i++) f.mask[i] = (bytes[(first + i) / 8] >> ((first + i) % 8)) & 1;
        else if (bytes && allocated == 8 && first + n <= length)
            for (size_t i = 0; i < n; i++) f.mask[i] = bytes[first + i] > 127;
        else
        {
            Slice s;
            if (!DecodeSlice(o.path, k, true, s).empty() || s.values.size() < n) continue;
            float most = 0;
            for (float v : s.values) most = (std::max)(most, v);
            for (size_t i = 0; i < n; i++) f.mask[i] = most > 0 && s.values[i] > most / 2;
        }
        f.any = std::find(f.mask.begin(), f.mask.end(), 1) != f.mask.end();
        double normal[3];
        NormalOf(f.plane.row, f.plane.column, normal);
        along.push_back(f.plane.origin[0] * normal[0] + f.plane.origin[1] * normal[1] + f.plane.origin[2] * normal[2]);
        o.frames.push_back(std::move(f));
    }
    // (the spacing of its slices: the median step between the distinct ones)
    std::sort(along.begin(), along.end());
    std::vector<double> steps;
    for (size_t i = 1; i < along.size(); i++) if (along[i] - along[i - 1] > 1e-3) steps.push_back(along[i] - along[i - 1]);
    std::sort(steps.begin(), steps.end());
    o.spacing = steps.empty() ? 0 : steps[steps.size() / 2];
    if (o.frames.empty()) o.failed = true;
}

bool PlaneOf(const Header& h, ImagePlane& p)
{
    return PlaneFrom(h.imageOrientation, h.imagePosition, h.pixelSpacing, static_cast<int>(h.columns),
                     static_cast<int>(h.rows), h.sliceThickness, p);
}

bool PlaneOf(DcmDataset* ds, int frame, ImagePlane& p)
{
    if (!ds) return false;
    DcmSequenceOfItems* perFrame = nullptr, *sharedSeq = nullptr;
    ds->findAndGetSequence(DCM_PerFrameFunctionalGroupsSequence, perFrame);
    DcmItem* item = perFrame && frame >= 0 && perFrame->card() > static_cast<unsigned long>(frame) ? perFrame->getItem(static_cast<unsigned long>(frame)) : nullptr;
    DcmItem* shared = ds->findAndGetSequence(DCM_SharedFunctionalGroupsSequence, sharedSeq).good() && sharedSeq && sharedSeq->card() ? sharedSeq->getItem(0) : nullptr;
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    return FramePlane(ds, item, shared, columns, rows, p);
}

bool SegmentsIn(const Segmentation& s, const ImagePlane& p)
{
    if (s.failed) return false;
    if (!s.rt)
    {
        const double tolerance = s.spacing > 0 ? s.spacing * 0.45 : 1.0;
        for (const SegFrame& f : s.frames) if (f.any && SamePlane(f.plane, p, tolerance)) return true;
        return false;
    }
    double normal[3];
    NormalOf(p.row, p.column, normal);
    auto gapOf = [&](const double* q) { double d = 0; for (int k = 0; k < 3; k++) d += (q[k] - p.origin[k]) * normal[k]; return d; };
    const double tolerance = p.thickness > 0 ? p.thickness * 0.5 : 1.0;
    for (const Contour& c : s.contours)
    {
        if (c.points.empty()) continue;
        bool in = true;
        for (size_t k = 0; k < c.points.size() && in; k += (std::max)(static_cast<size_t>(1), c.points.size() / 4))
            if (std::fabs(gapOf(c.points[k].data())) > tolerance) in = false;
        if (in) return true;
    }
    return false;
}

void SegmentsOver(const std::vector<const Segmentation*>& all, const ImagePlane& p, std::vector<DWORD>& argb,
                  std::vector<SegOutline>& outlines)
{
    argb.clear();
    outlines.clear();
    const int width = p.columns, height = p.rows;
    if (width <= 0 || height <= 0) return;
    double normal[3];
    NormalOf(p.row, p.column, normal);
    std::vector<int> label(static_cast<size_t>(width) * height, 0);
    std::vector<COLORREF> palette(1, 0);
    for (const Segmentation* o : all)
    {
        if (!o || o->failed) continue;
        if (!o->rt)
        {
            const double tolerance = o->spacing > 0 ? o->spacing * 0.45 : 1.0;
            for (const SegFrame& f : o->frames)
            {
                const ImagePlane& q = f.plane;
                if (!SamePlane(q, p, tolerance)) continue;
                auto it = o->colors.find(f.segment);
                palette.push_back(it != o->colors.end() ? it->second : Palette[(5 + f.segment) % PaletteSize]);
                const int id = static_cast<int>(palette.size()) - 1;
                // Each pixel of the image, where it is in the frame (its nearest pixel there)
                double u0 = 0, v0 = 0, uc = 0, vc = 0, ur = 0, vr = 0;
                for (int k = 0; k < 3; k++)
                {
                    const double d = p.origin[k] - q.origin[k];
                    u0 += d * q.row[k];
                    v0 += d * q.column[k];
                    uc += p.dx * p.row[k] * q.row[k];
                    vc += p.dx * p.row[k] * q.column[k];
                    ur += p.dy * p.column[k] * q.row[k];
                    vr += p.dy * p.column[k] * q.column[k];
                }
                for (int r = 0; r < height; r++)
                    for (int c = 0; c < width; c++)
                    {
                        const long u = std::lround((u0 + c * uc + r * ur) / q.dx), v = std::lround((v0 + c * vc + r * vr) / q.dy);
                        if (u < 0 || v < 0 || u >= q.columns || v >= q.rows) continue;
                        if (f.mask[static_cast<size_t>(v) * q.columns + u]) label[static_cast<size_t>(r) * width + c] = id;
                    }
            }
            continue;
        }
        // A structure set: its contours in the plane of the image, over it
        const double tolerance = p.thickness > 0 ? p.thickness * 0.5 : 1.0;
        for (const Contour& c3 : o->contours)
        {
            bool inPlane = true;
            SegOutline out;
            out.color = c3.color;
            out.closed = c3.closed;
            for (const auto& q : c3.points)
            {
                double gap = 0, u = 0, v = 0;
                for (int k = 0; k < 3; k++)
                {
                    const double d = q[k] - p.origin[k];
                    gap += d * normal[k];
                    u += d * p.row[k];
                    v += d * p.column[k];
                }
                if (std::fabs(gap) > tolerance) { inPlane = false; break; }
                out.points.push_back({ u / p.dx, v / p.dy });
            }
            if (inPlane && !out.points.empty()) outlines.push_back(std::move(out));
        }
    }
    // The mask: its color, faint inside and strong at its edge
    if (palette.size() < 2) return;
    argb.assign(label.size(), 0);
    for (int r = 0; r < height; r++)
        for (int c = 0; c < width; c++)
        {
            const int id = label[static_cast<size_t>(r) * width + c];
            if (!id) continue;
            auto other = [&](int rr, int cc) { return rr < 0 || cc < 0 || rr >= height || cc >= width || label[static_cast<size_t>(rr) * width + cc] != id; };
            const bool edge = other(r - 1, c) || other(r + 1, c) || other(r, c - 1) || other(r, c + 1);
            const DWORD alpha = edge ? 235 : 80;
            const COLORREF col = palette[id];
            const DWORD R = GetRValue(col) * alpha / 255, G = GetGValue(col) * alpha / 255, B = GetBValue(col) * alpha / 255;
            argb[static_cast<size_t>(r) * width + c] = (alpha << 24) | (R << 16) | (G << 8) | B;
        }
}

// ----- The segmentations near a file: found once, and again when the folders change (their last write times,
// looked at every few seconds)

static std::wstring LowerPath(std::wstring s)
{
    if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

static std::wstring Joined(const std::wstring& folder, const wchar_t* name)
{
    return folder + (folder.back() == L'\\' ? L"" : L"\\") + name;
}

// The folders in a folder (not hidden or offline), and a line for each one with its last write time
static void FoldersIn(const std::wstring& folder, std::vector<std::wstring>& out, std::wstring& signature)
{
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(Joined(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_OFFLINE)) ||
            !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (out.size() >= 400) break;
        out.push_back(Joined(folder, fd.cFileName));
        wchar_t written[40];
        swprintf_s(written, L"|%08X%08X\n", fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
        signature += out.back() + written;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

// The SEG and RT Structure Set files of a folder: of all its files, or (a folder of a series) only when its
// first DICOM file is one
static void SegmentationsIn(const std::wstring& folder, bool all, std::vector<std::shared_ptr<Segmentation>>& found)
{
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(Joined(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    int files = 0;
    do
    {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) continue;
        if (++files > 20000) break;
        const std::wstring path = Joined(folder, fd.cFileName);
        Header h;
        if (ReadHeader(path.c_str(), h, 0x0020000D) != Kind::Dicom) continue;
        const bool seg = h.sopClass == SegSopClass, rt = h.sopClass == RtStructSopClass;
        if (!seg && !rt)
        {
            if (all) continue;
            break;
        }
        all = true;
        auto s = std::make_shared<Segmentation>();
        s->path = path;
        s->rt = rt;
        s->study = h.studyUid;
        found.push_back(std::move(s));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

struct Nearby
{
    std::wstring signature;
    ULONGLONG checked = 0;
    std::vector<std::shared_ptr<Segmentation>> found;
};

static std::mutex g_nearLock;                       // the folders known, and the reading of their segmentations
static std::unordered_map<std::wstring, Nearby> g_near;     // (lowercase folder)

std::vector<std::shared_ptr<Segmentation>> SegmentationsNear(const std::wstring& file, const std::string& study)
{
    std::vector<std::shared_ptr<Segmentation>> out;
    const size_t slash = file.find_last_of(L'\\');
    if (slash == std::wstring::npos || study.empty()) return out;
    const std::wstring folder = file.substr(0, slash == 2 ? 3 : slash);
    std::wstring parent;
    if (folder.size() > 3)
    {
        const size_t up = folder.find_last_of(L'\\');
        if (up != std::wstring::npos) parent = folder.substr(0, up == 2 ? 3 : up);
    }
    std::lock_guard<std::mutex> hold(g_nearLock);
    const std::wstring key = LowerPath(folder);
    if (g_near.size() >= 16 && !g_near.count(key)) g_near.clear();
    Nearby& n = g_near[key];
    const ULONGLONG now = GetTickCount64();
    if (!n.checked || now - n.checked > 3000)
    {
        n.checked = now;
        std::wstring signature;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(folder.c_str(), GetFileExInfoStandard, &a))
        {
            wchar_t written[40];
            swprintf_s(written, L"%08X%08X\n", a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
            signature = written;
        }
        std::vector<std::wstring> inside, beside;
        FoldersIn(folder, inside, signature);
        if (!parent.empty()) FoldersIn(parent, beside, signature);
        if (signature != n.signature)
        {
            n.signature = signature;
            n.found.clear();
            SegmentationsIn(folder, true, n.found);
            for (const std::wstring& f : inside) SegmentationsIn(f, false, n.found);
            for (const std::wstring& f : beside) if (LowerPath(f) != key) SegmentationsIn(f, false, n.found);
        }
    }
    for (const auto& s : n.found)
    {
        if (s->study != study) continue;
        if (!s->loaded)
        {
            try { LoadSegmentation(*s); }
            catch (...) { s->failed = true; }
        }
        if (!s->failed) out.push_back(s);
    }
    return out;
}
