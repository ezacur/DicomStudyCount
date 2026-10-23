// DicomIcons: Explorer icon handler for DICOM files. Every DICOM file gets an icon with its
// SeriesNumber written in the color of its study (the same palette and hash as the folder boxes of
// Count-DicomStudies.ps1), on the complementary color. Files that are not DICOM (or are a DICOMDIR)
// keep the generic file icon.
//
// Only the header is read, up to SeriesNumber (0020,0011), the same way as the script's reader:
// Explicit/Implicit VR Little Endian, Explicit VR Big Endian and "raw" files without preamble.
// Deflated files are DICOM but are not decompressed: they get a "?".
//
// It is also the property handler of DICOM files: the DicomStudyCount.* columns of Explorer
// (DicomStudyCount.propdesc), read from the same header, plus the copies of every file found by the
// last count ("duplicated in").
//
// Registered by Count-DicomStudies.ps1 -Install for .dcm and files without extension: the icons per
// user (Register-DicomIcons.ps1), the properties for the machine (Register-DicomProperties.ps1).
// It runs inside Explorer: every error ends in the generic icon or no properties, never in an exception.

#include <windows.h>
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <cmath>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#include "PlaneClouds.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "propsys.lib")

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

// {F4798986-2EEA-49F6-8082-2FC552CE691B}
static const CLSID CLSID_DicomIcons = { 0xF4798986, 0x2EEA, 0x49F6, { 0x80, 0x82, 0x2F, 0xC5, 0x52, 0xCE, 0x69, 0x1B } };
// {21329A2B-09B3-4250-9103-9267877321EB}
static const CLSID CLSID_DicomProperties = { 0x21329A2B, 0x09B3, 0x4250, { 0x91, 0x03, 0x92, 0x67, 0x87, 0x73, 0x21, 0xEB } };

static LONG g_locks = 0;            // live objects and LockServer calls

// Icon cache key: changes when the drawing changes, so Explorer never mixes old and new icons
static const wchar_t KeyPrefix[] = L"DicomStudyCount|v2|";

// ---------------------------------------------------------------------------------------------
// Header reader

class File
{
public:
    explicit File(const wchar_t* path)
    {
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER s;
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &s)) size = s.QuadPart;
    }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }

    bool Ok() const { return h != INVALID_HANDLE_VALUE; }
    long long Size() const { return size; }
    long long Pos() const { return pos; }
    void Seek(long long p) { pos = p; }

    bool Read(void* dst, size_t n)
    {
        auto out = static_cast<unsigned char*>(dst);
        while (n > 0)
        {
            if (pos < bufStart || pos >= bufStart + bufLen)
            {
                LARGE_INTEGER at; at.QuadPart = pos;
                DWORD got = 0;
                if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0)
                    return false;
                bufStart = pos; bufLen = got;
            }
            size_t off = static_cast<size_t>(pos - bufStart);
            size_t take = static_cast<size_t>(bufLen) - off;
            if (take > n) take = n;
            memcpy(out, buf + off, take);
            out += take; n -= take; pos += take;
        }
        return true;
    }

    bool Skip(unsigned long long n)
    {
        if (pos + static_cast<long long>(n) > size) return false;
        pos += static_cast<long long>(n);
        return true;
    }

private:
    HANDLE h = INVALID_HANDLE_VALUE;
    long long size = 0, pos = 0, bufStart = 0;
    DWORD bufLen = 0;
    unsigned char buf[65536];
};

enum class Kind { NotDicom, Dicom };

struct Header
{
    std::string charset, studyUid, seriesNumber, seriesUid, seriesDescription, modality, sopUid;
    std::string patientId, instanceNumber, imagePosition, imageOrientation;
    unsigned rows = 0, columns = 0;                     // 0: not in the header
    std::string transferSyntax, sopClass, imageType, manufacturer, protocolName, frameOfReference;
    std::string acquisitionDateTime, acquisitionDate, acquisitionTime, contentDate, contentTime;
    std::string seriesDate, seriesTime, studyDate;
    std::string numberOfFrames, triggerTime, sliceThickness, pixelSpacing;
};

// Where the reading stops: the icons need the header up to SeriesNumber only, the columns up to
// PixelSpacing (the last attribute they show)
static const unsigned UpToSeriesNumber = 0x00200011;
static const unsigned UpToPixelSpacing = 0x00280030;

class Parser
{
public:
    explicit Parser(File& f) : f(f) {}
    bool expl = true, big = false;

    bool U16(unsigned& v)
    {
        unsigned char b[2];
        if (!f.Read(b, 2)) return false;
        v = big ? (b[0] << 8) | b[1] : b[0] | (b[1] << 8);
        return true;
    }

    bool U32(unsigned& v)
    {
        unsigned char b[4];
        if (!f.Read(b, 4)) return false;
        v = big ? (unsigned(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
                : b[0] | (b[1] << 8) | (b[2] << 16) | (unsigned(b[3]) << 24);
        return true;
    }

    bool Tag(unsigned& tag)
    {
        unsigned g, e;
        if (!U16(g) || !U16(e)) return false;
        tag = (g << 16) | e;
        return true;
    }

    bool Length(unsigned tag, char vr[2], unsigned& len)
    {
        vr[0] = vr[1] = 0;
        if (expl && (tag >> 16) != 0xFFFE)
        {
            if (!f.Read(vr, 2)) return false;
            if (LongLength(vr)) { unsigned skip; return U16(skip) && U32(len); }
            unsigned l16;
            if (!U16(l16)) return false;
            len = l16;
            return true;
        }
        return U32(len);
    }

    bool String(unsigned len, std::string& value)
    {
        if (len > 4096) return f.Skip(len);
        value.resize(len);
        if (len && !f.Read(&value[0], len)) return false;
        size_t a = value.find_first_not_of(std::string(" \0", 2));
        size_t z = value.find_last_not_of(std::string(" \0", 2));
        value = a == std::string::npos ? std::string() : value.substr(a, z - a + 1);
        return true;
    }

    // A sequence of undefined length (a UN of undefined length is Implicit VR Little Endian)
    bool SkipSequence(const char vr[2], int depth = 0)
    {
        if (depth > 32) return false;
        bool saveExpl = expl, saveBig = big;
        if (vr[0] == 'U' && vr[1] == 'N') { expl = false; big = false; }
        bool ok = true;
        for (;;)
        {
            unsigned tag, len;
            if (!Tag(tag) || !U32(len)) { ok = false; break; }
            if (tag == 0xFFFEE0DD) break;                          // Sequence Delimitation
            if (tag != 0xFFFEE000) { ok = false; break; }
            if (len == 0xFFFFFFFF) { if (!SkipItem(depth + 1)) { ok = false; break; } }
            else if (!f.Skip(len)) { ok = false; break; }
        }
        expl = saveExpl; big = saveBig;
        return ok;
    }

private:
    File& f;

    bool SkipItem(int depth)
    {
        for (;;)
        {
            unsigned tag, len;
            if (!Tag(tag)) return false;
            if (tag == 0xFFFEE00D) { unsigned z; return U32(z); }   // Item Delimitation
            char vr[2];
            if (!Length(tag, vr, len)) return false;
            if (len == 0xFFFFFFFF) { if (!SkipSequence(vr, depth)) return false; }
            else if (!f.Skip(len)) return false;
        }
    }

    static bool LongLength(const char vr[2])
    {
        static const char* longVrs[] = { "OB", "OD", "OF", "OL", "OV", "OW", "SQ", "SV", "UC", "UN", "UR", "UT", "UV" };
        for (auto v : longVrs) if (vr[0] == v[0] && vr[1] == v[1]) return true;
        return false;
    }
};

static const char DirectorySopClass[] = "1.2.840.10008.1.3.10";   // DICOMDIR

static Kind ReadDataset(Parser& p, File& f, Header& h, unsigned last)
{
    while (f.Pos() + 4 <= f.Size())
    {
        unsigned tag, len;
        if (!p.Tag(tag)) break;
        if (tag > last) break;                                     // sorted: the rest is behind
        char vr[2];
        if (!p.Length(tag, vr, len)) break;
        if (tag == 0x00041220) return Kind::NotDicom;              // directory records: a DICOMDIR
        if (len == 0xFFFFFFFF) { if (!p.SkipSequence(vr)) break; continue; }
        switch (tag)
        {
        case 0x00080016:                                           // SOPClassUID
            if (!p.String(len, h.sopClass)) return Kind::Dicom;
            if (h.sopClass == DirectorySopClass) return Kind::NotDicom;
            break;
        case 0x00080008:                                           // ImageType
            if (!p.String(len, h.imageType)) return Kind::Dicom;
            break;
        case 0x00080020:                                           // StudyDate
            if (!p.String(len, h.studyDate)) return Kind::Dicom;
            break;
        case 0x00080021:                                           // SeriesDate
            if (!p.String(len, h.seriesDate)) return Kind::Dicom;
            break;
        case 0x00080022:                                           // AcquisitionDate
            if (!p.String(len, h.acquisitionDate)) return Kind::Dicom;
            break;
        case 0x00080023:                                           // ContentDate
            if (!p.String(len, h.contentDate)) return Kind::Dicom;
            break;
        case 0x0008002A:                                           // AcquisitionDateTime
            if (!p.String(len, h.acquisitionDateTime)) return Kind::Dicom;
            break;
        case 0x00080031:                                           // SeriesTime
            if (!p.String(len, h.seriesTime)) return Kind::Dicom;
            break;
        case 0x00080032:                                           // AcquisitionTime
            if (!p.String(len, h.acquisitionTime)) return Kind::Dicom;
            break;
        case 0x00080033:                                           // ContentTime
            if (!p.String(len, h.contentTime)) return Kind::Dicom;
            break;
        case 0x00080070:                                           // Manufacturer
            if (!p.String(len, h.manufacturer)) return Kind::Dicom;
            break;
        case 0x00180050:                                           // SliceThickness
            if (!p.String(len, h.sliceThickness)) return Kind::Dicom;
            break;
        case 0x00181030:                                           // ProtocolName
            if (!p.String(len, h.protocolName)) return Kind::Dicom;
            break;
        case 0x00181060:                                           // TriggerTime
            if (!p.String(len, h.triggerTime)) return Kind::Dicom;
            break;
        case 0x00200052:                                           // FrameOfReferenceUID
            if (!p.String(len, h.frameOfReference)) return Kind::Dicom;
            break;
        case 0x00280008:                                           // NumberOfFrames
            if (!p.String(len, h.numberOfFrames)) return Kind::Dicom;
            break;
        case 0x00280030:                                           // PixelSpacing
            if (!p.String(len, h.pixelSpacing)) return Kind::Dicom;
            break;
        case 0x00080005:                                           // SpecificCharacterSet
            if (!p.String(len, h.charset)) return Kind::Dicom;
            break;
        case 0x00080018:                                           // SOPInstanceUID
            if (!p.String(len, h.sopUid)) return Kind::Dicom;
            break;
        case 0x00080060:                                           // Modality
            if (!p.String(len, h.modality)) return Kind::Dicom;
            break;
        case 0x0008103E:                                           // SeriesDescription
            if (!p.String(len, h.seriesDescription)) return Kind::Dicom;
            break;
        case 0x0020000D:                                           // StudyInstanceUID
            if (!p.String(len, h.studyUid)) return Kind::Dicom;
            break;
        case 0x0020000E:                                           // SeriesInstanceUID
            if (!p.String(len, h.seriesUid)) return Kind::Dicom;
            break;
        case 0x00100020:                                           // PatientID
            if (!p.String(len, h.patientId)) return Kind::Dicom;
            break;
        case 0x00200011:                                           // SeriesNumber
            if (!p.String(len, h.seriesNumber)) return Kind::Dicom;
            break;
        case 0x00200013:                                           // InstanceNumber
            if (!p.String(len, h.instanceNumber)) return Kind::Dicom;
            break;
        case 0x00200032:                                           // ImagePositionPatient
            if (!p.String(len, h.imagePosition)) return Kind::Dicom;
            break;
        case 0x00200037:                                           // ImageOrientationPatient
            if (!p.String(len, h.imageOrientation)) return Kind::Dicom;
            break;
        case 0x00280010:                                           // Rows (US, binary)
        case 0x00280011:                                           // Columns
        {
            unsigned value = 0;
            if (len < 2 || !p.U16(value) || !f.Skip(len - 2)) return Kind::Dicom;
            (tag == 0x00280010 ? h.rows : h.columns) = value;
            break;
        }
        default:
            if (!f.Skip(len)) return Kind::Dicom;
        }
        if (tag == last) break;
    }
    return Kind::Dicom;
}

// Meta information (group 0002, always Explicit VR Little Endian), then the dataset
static Kind ReadAfterPreamble(File& f, Header& h, bool raw, unsigned last)
{
    Parser p(f);
    std::string ts, mediaClass;
    while (f.Pos() + 8 <= f.Size())
    {
        long long at = f.Pos();
        unsigned tag, len;
        if (!p.Tag(tag)) return Kind::NotDicom;
        if ((tag >> 16) != 0x0002) { f.Seek(at); break; }
        char vr[2];
        if (!p.Length(tag, vr, len)) return Kind::NotDicom;
        if (tag == 0x00020010) { if (!p.String(len, ts)) return Kind::NotDicom; }
        else if (tag == 0x00020002) { if (!p.String(len, mediaClass)) return Kind::NotDicom; }
        else if (!f.Skip(len)) return Kind::NotDicom;
    }
    if (mediaClass == DirectorySopClass) return Kind::NotDicom;
    h.transferSyntax = ts;

    if (ts == "1.2.840.10008.1.2") p.expl = false;                 // Implicit VR Little Endian
    else if (ts == "1.2.840.10008.1.2.2") p.big = true;            // Explicit VR Big Endian
    else if (ts == "1.2.840.10008.1.2.1.99") return Kind::Dicom;   // Deflated: not decompressed here
    else if (ts.empty())
    {
        // No meta information: Explicit if the bytes after the first tag look like a VR
        long long at = f.Pos();
        unsigned char b[6];
        p.expl = f.Read(b, 6) && b[4] >= 'A' && b[4] <= 'Z' && b[5] >= 'A' && b[5] <= 'Z';
        f.Seek(at);
    }
    Kind k = ReadDataset(p, f, h, last);
    // A raw file (no preamble) only counts as DICOM with a StudyInstanceUID
    if (raw && h.studyUid.empty()) return Kind::NotDicom;
    return k;
}

static Kind ReadHeader(const wchar_t* path, Header& h, unsigned last)
{
    File f(path);
    if (!f.Ok()) return Kind::NotDicom;
    unsigned char head[132];
    bool full = f.Size() >= 132 && f.Read(head, 132);
    if (full && head[128] == 'D' && head[129] == 'I' && head[130] == 'C' && head[131] == 'M')
        return ReadAfterPreamble(f, h, false, last);
    // "Raw" DICOM without preamble: only attempted if it starts with group 0002 or 0008
    f.Seek(0);
    unsigned char first[8];
    if (f.Size() < 8 || !f.Read(first, 8) || first[1] != 0 || (first[0] != 0x02 && first[0] != 0x08)) return Kind::NotDicom;
    f.Seek(0);
    return ReadAfterPreamble(f, h, true, last);
}

// ---------------------------------------------------------------------------------------------
// Colors: the palette and hash of FolderIcons in Count-DicomStudies.ps1

static const unsigned Palette[] = {                     // 0xRRGGBB
    0x000000, 0x404040, 0x808080, 0xBFBFBF, 0xFFFFFF,   // black, gray 25/50/75 %, white
    0xE6194B, 0x800000, 0xF58231, 0xFFD8B1, 0x9A6324,   // red, maroon, orange, apricot, brown
    0x808000, 0xBFEF45, 0x3CB44B, 0xAAFFC3, 0x469990,   // olive, lime, green, mint, teal
    0x42D4F4, 0x4363D8, 0x000075, 0xDCBEFF, 0x911EB4,   // cyan, blue, navy, lavender, purple
    0xF032E6, 0xFABED4                                  // magenta, pink
};
static const int PaletteSize = sizeof(Palette) / sizeof(Palette[0]);
static const int NoStudyColor = 2;                      // gray: DICOM without StudyInstanceUID

static int ColorIndex(const std::string& uid)
{
    unsigned long long h = 14695981039346656037ULL;     // 64-bit FNV-1a, as ColorIndex in C#
    for (unsigned char c : uid) { h ^= c; h *= 1099511628211ULL; }
    return static_cast<int>((h >> 32) % PaletteSize);
}

struct Rgb { double r, g, b; };
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
static Rgb Background(Rgb text)
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

static HICON DrawIcon(int size, int color, const std::wstring& label)
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

    Rgb text = FromHex(Palette[color]);
    Rgb bg = Background(text);
    RECT all = { 0, 0, size, size };
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
        int x = (size - g.width) / 2;
        int y = (size - g.digitHeight + 1) / 2 + g.digitTop - g.ascent;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToGdi(text));
        TextOutW(dc, x, y, label.c_str(), static_cast<int>(label.size()));
        SelectObject(dc, old);
        DeleteObject(font);
    }
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

class IconHandler : public IPersistFile, public IExtractIconW
{
public:
    IconHandler() { InterlockedIncrement(&g_locks); }
    virtual ~IconHandler() { InterlockedDecrement(&g_locks); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IExtractIconW) *ppv = static_cast<IExtractIconW*>(this);
        else if (riid == IID_IPersist || riid == IID_IPersistFile) *ppv = static_cast<IPersistFile*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return r;
    }

    // IPersistFile
    IFACEMETHODIMP GetClassID(CLSID* clsid) override { if (!clsid) return E_POINTER; *clsid = CLSID_DicomIcons; return S_OK; }
    IFACEMETHODIMP IsDirty() override { return S_FALSE; }
    IFACEMETHODIMP Load(LPCOLESTR file, DWORD) override
    {
        if (!file) return E_INVALIDARG;
        try { path = file; } catch (...) { return E_OUTOFMEMORY; }
        return S_OK;
    }
    IFACEMETHODIMP Save(LPCOLESTR, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP SaveCompleted(LPCOLESTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCurFile(LPOLESTR*) override { return E_NOTIMPL; }

    // IExtractIconW: the location is a key ("DicomStudyCount|v1|<color>|<number>") that Explorer
    // caches the drawn icon under, so every color and number is drawn only once
    IFACEMETHODIMP GetIconLocation(UINT flags, LPWSTR file, UINT cch, int* index, UINT* outFlags) override
    {
        if (!file || !index || !outFlags || cch == 0) return E_INVALIDARG;
        // Reading the file can be slow (network): Explorer calls again on a background thread
        if (flags & GIL_ASYNC) return E_PENDING;
        try
        {
            Header h;
            if (path.empty() || ReadHeader(path.c_str(), h, UpToSeriesNumber) != Kind::Dicom)
            {
                // Not DICOM: the generic file icon
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

    IFACEMETHODIMP Extract(LPCWSTR file, UINT, HICON* largeIcon, HICON* smallIcon, UINT sizes) override
    {
        if (!file) return E_INVALIDARG;
        try
        {
            std::wstring key = file;
            size_t prefix = wcslen(KeyPrefix);
            if (key.compare(0, prefix, KeyPrefix) != 0) return S_FALSE;    // the generic icon: Explorer extracts it
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

private:
    LONG refs = 1;
    std::wstring path;
};

// DICOM text: UTF-8 when the SpecificCharacterSet says ISO_IR 192, otherwise read as Latin-1 (as the
// script's reader does: one byte, one character)
static std::wstring Widen(const std::string& s, const std::string& charset = std::string())
{
    std::wstring w;
    if (charset.find("ISO_IR 192") != std::string::npos && !s.empty())
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n > 0)
        {
            w.resize(n);
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
            return w;
        }
    }
    w.reserve(s.size());
    for (unsigned char c : s) w += static_cast<wchar_t>(c);
    return w;
}

// 5 case-sensitive characters from the 64-bit FNV-1a of a UID: for the StudyInstanceUID, the same as
// the study tags of the folder comments ("[MR-hwmA0]"), FolderComments.StudyTag in Count-DicomStudies.ps1
static std::wstring Hash5(const std::string& uid)
{
    static const wchar_t Base62[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned long long h = 14695981039346656037ULL;
    for (unsigned char c : uid) { h ^= c; h *= 1099511628211ULL; }
    unsigned long long v = h % 916132832ULL;                // 62^5
    wchar_t chars[6] = {};
    for (int i = 4; i >= 0; i--) { chars[i] = Base62[v % 62]; v /= 62; }
    return chars;
}

// Copies of every file found by the counts, read from %LOCALAPPDATA%\DicomStudyCount\duplicates
// (one file per counted folder: "path<TAB>other copy<TAB>..."). Reloaded when those files change,
// checked at most every 2 seconds; shared by all the property handlers of the process.
class Duplicates
{
public:
    std::wstring Of(const std::wstring& path)
    {
        Refresh();
        AcquireSRWLockShared(&lock);
        std::wstring result;
        auto it = copies.find(Lower(path));
        if (it != copies.end()) result = it->second;
        ReleaseSRWLockShared(&lock);
        return result;
    }

private:
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, std::wstring> copies;      // lowercase path -> "copy; copy"
    std::wstring signature;                                     // names, sizes and times of the files read
    ULONGLONG checkedAt = 0;
    LONG refreshing = 0;

    static std::wstring Lower(std::wstring s)
    {
        if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
        return s;
    }

    static std::wstring Folder()
    {
        PWSTR local = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\DicomStudyCount\\duplicates";
        CoTaskMemFree(local);
        return dir;
    }

    void Refresh()
    {
        ULONGLONG now = GetTickCount64();
        if (now - checkedAt < 2000 && checkedAt != 0) return;
        if (InterlockedExchange(&refreshing, 1)) return;        // another thread is at it
        checkedAt = now;
        std::wstring dir = Folder();
        std::wstring sig;
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        HANDLE find = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"\\*.tsv").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                files.push_back(dir + L"\\" + fd.cFileName);
                sig += std::wstring(fd.cFileName) + L"|" + std::to_wstring(fd.nFileSizeLow) + L"|" +
                       std::to_wstring(fd.ftLastWriteTime.dwLowDateTime) + L"|" + std::to_wstring(fd.ftLastWriteTime.dwHighDateTime) + L";";
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        if (sig != signature)
        {
            std::unordered_map<std::wstring, std::wstring> loaded;
            for (auto& f : files) Load(f, loaded);
            AcquireSRWLockExclusive(&lock);
            copies.swap(loaded);
            signature = sig;
            ReleaseSRWLockExclusive(&lock);
        }
        InterlockedExchange(&refreshing, 0);
    }

    static void Load(const std::wstring& file, std::unordered_map<std::wstring, std::wstring>& into)
    {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size;
        std::string text;
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (256LL << 20))
        {
            text.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            if (!ReadFile(h, &text[0], static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
            text.resize(got);
        }
        CloseHandle(h);
        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (n <= 0) return;
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &w[0], n);
        size_t at = 0;
        while (at < w.size())
        {
            size_t end = w.find(L'\n', at);
            if (end == std::wstring::npos) end = w.size();
            std::wstring line = w.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty() || line[0] == L'#') continue;
            size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos) continue;
            std::wstring others = line.substr(tab + 1);
            for (auto& c : others) if (c == L'\t') c = L';';
            size_t semi = 0;
            while ((semi = others.find(L';', semi)) != std::wstring::npos) { others.insert(semi + 1, L" "); semi += 2; }
            into[Lower(line.substr(0, tab))] = others;
        }
    }
};

static Duplicates g_duplicates;

// Our columns (DicomStudyCount.propdesc)
static const GUID FMTID_Dicom = { 0xED9AB6BD, 0x5610, 0x4A64, { 0xB2, 0xCC, 0x55, 0xEE, 0x8E, 0x18, 0xD3, 0x29 } };
static const PROPERTYKEY PKEY_StudyHash = { FMTID_Dicom, 2 };
static const PROPERTYKEY PKEY_StudyUid = { FMTID_Dicom, 3 };
static const PROPERTYKEY PKEY_SeriesUid = { FMTID_Dicom, 4 };
static const PROPERTYKEY PKEY_SeriesNumber = { FMTID_Dicom, 5 };
static const PROPERTYKEY PKEY_SeriesDescription = { FMTID_Dicom, 6 };
static const PROPERTYKEY PKEY_Modality = { FMTID_Dicom, 7 };
static const PROPERTYKEY PKEY_DuplicatedIn = { FMTID_Dicom, 8 };
static const PROPERTYKEY PKEY_SeriesHash = { FMTID_Dicom, 9 };
static const PROPERTYKEY PKEY_Rows = { FMTID_Dicom, 10 };
static const PROPERTYKEY PKEY_Columns = { FMTID_Dicom, 11 };
static const PROPERTYKEY PKEY_ImageOrientation = { FMTID_Dicom, 12 };
static const PROPERTYKEY PKEY_InstanceNumber = { FMTID_Dicom, 13 };
static const PROPERTYKEY PKEY_PatientID = { FMTID_Dicom, 14 };
static const PROPERTYKEY PKEY_ZLevel = { FMTID_Dicom, 15 };

// The numbers of a DS / IS value ("1\0\0\0\1\0"); false unless there are exactly n of them, all finite
static bool Numbers(const std::string& value, double* out, int n)
{
    const char* s = value.c_str();
    for (int i = 0; i < n; i++)
    {
        char* end = nullptr;
        out[i] = strtod(s, &end);
        if (end == s || !std::isfinite(out[i])) return false;
        while (*end == ' ') end++;
        if (i < n - 1) { if (*end != '\\') return false; s = end + 1; }
        else if (*end != 0) return false;
    }
    return true;
}

// Z level of the slice, as xZ in add_xR_and_xZ of DICOMstack.m: R has the columns X and Y of
// ImageOrientationPatient and n = X x Y (taken before normalizing), each one normalized (5 times,
// as there, so that the norm is exactly 1); z = third row of inv(R) = R' times ImagePositionPatient,
// i.e. the position along the normal of the slice
static bool ZLevel(const Header& h, double& z)
{
    double o[6], p[3];
    if (!Numbers(h.imageOrientation, o, 6) || !Numbers(h.imagePosition, p, 3)) return false;
    double R[3][3] = { { o[0], o[3], 0 }, { o[1], o[4], 0 }, { o[2], o[5], 0 } };    // R[row][column]
    R[0][2] = R[1][0] * R[2][1] - R[2][0] * R[1][1];
    R[1][2] = R[2][0] * R[0][1] - R[0][0] * R[2][1];
    R[2][2] = R[0][0] * R[1][1] - R[1][0] * R[0][1];
    for (int c = 0; c < 3; c++)
        for (int it = 0; it < 5; it++)
        {
            double norm = sqrt(R[0][c] * R[0][c] + R[1][c] * R[1][c] + R[2][c] * R[2][c]);
            if (!(norm > 0)) return false;
            for (int r = 0; r < 3; r++) R[r][c] /= norm;
        }
    z = R[0][2] * p[0] + R[1][2] * p[1] + R[2][2] * p[2];
    return std::isfinite(z);
}

static const PROPERTYKEY PKEY_Acquired = { FMTID_Dicom, 16 };
static const PROPERTYKEY PKEY_StudyDate = { FMTID_Dicom, 17 };
static const PROPERTYKEY PKEY_FrameOfReference = { FMTID_Dicom, 18 };
static const PROPERTYKEY PKEY_ObjectType = { FMTID_Dicom, 19 };
static const PROPERTYKEY PKEY_ImageType = { FMTID_Dicom, 20 };
static const PROPERTYKEY PKEY_Frames = { FMTID_Dicom, 21 };
static const PROPERTYKEY PKEY_TriggerTime = { FMTID_Dicom, 22 };
static const PROPERTYKEY PKEY_PixelSpacing = { FMTID_Dicom, 23 };
static const PROPERTYKEY PKEY_SliceThickness = { FMTID_Dicom, 24 };
static const PROPERTYKEY PKEY_Compression = { FMTID_Dicom, 25 };
static const PROPERTYKEY PKEY_Protocol = { FMTID_Dicom, 26 };
static const PROPERTYKEY PKEY_Manufacturer = { FMTID_Dicom, 27 };
static const PROPERTYKEY PKEY_Plane = { FMTID_Dicom, 28 };

// The most common SOP classes by name; any other one as its UID
static std::wstring ObjectType(const std::string& uid)
{
    static const struct { const char* uid; const wchar_t* name; } names[] = {
        { "1.2.840.10008.5.1.4.1.1.1", L"CR Image" },
        { "1.2.840.10008.5.1.4.1.1.1.1", L"Digital X-Ray Image" },
        { "1.2.840.10008.5.1.4.1.1.1.1.1", L"Digital X-Ray Image (processing)" },
        { "1.2.840.10008.5.1.4.1.1.1.2", L"Digital Mammography Image" },
        { "1.2.840.10008.5.1.4.1.1.2", L"CT Image" },
        { "1.2.840.10008.5.1.4.1.1.2.1", L"Enhanced CT Image" },
        { "1.2.840.10008.5.1.4.1.1.3.1", L"US Multi-frame Image" },
        { "1.2.840.10008.5.1.4.1.1.4", L"MR Image" },
        { "1.2.840.10008.5.1.4.1.1.4.1", L"Enhanced MR Image" },
        { "1.2.840.10008.5.1.4.1.1.4.2", L"MR Spectroscopy" },
        { "1.2.840.10008.5.1.4.1.1.4.3", L"Enhanced MR Color Image" },
        { "1.2.840.10008.5.1.4.1.1.6.1", L"US Image" },
        { "1.2.840.10008.5.1.4.1.1.7", L"Secondary Capture" },
        { "1.2.840.10008.5.1.4.1.1.7.1", L"Multi-frame Secondary Capture (bit)" },
        { "1.2.840.10008.5.1.4.1.1.7.2", L"Multi-frame Secondary Capture (byte)" },
        { "1.2.840.10008.5.1.4.1.1.7.3", L"Multi-frame Secondary Capture (word)" },
        { "1.2.840.10008.5.1.4.1.1.7.4", L"Multi-frame Secondary Capture (color)" },
        { "1.2.840.10008.5.1.4.1.1.9.1.1", L"12-lead ECG" },
        { "1.2.840.10008.5.1.4.1.1.11.1", L"Grayscale Presentation State" },
        { "1.2.840.10008.5.1.4.1.1.11.2", L"Color Presentation State" },
        { "1.2.840.10008.5.1.4.1.1.12.1", L"X-Ray Angiographic Image" },
        { "1.2.840.10008.5.1.4.1.1.12.2", L"X-Ray Radiofluoroscopic Image" },
        { "1.2.840.10008.5.1.4.1.1.13.1.3", L"Breast Tomosynthesis Image" },
        { "1.2.840.10008.5.1.4.1.1.20", L"NM Image" },
        { "1.2.840.10008.5.1.4.1.1.66", L"Raw Data" },
        { "1.2.840.10008.5.1.4.1.1.66.1", L"Spatial Registration" },
        { "1.2.840.10008.5.1.4.1.1.66.4", L"Segmentation" },
        { "1.2.840.10008.5.1.4.1.1.66.5", L"Surface Segmentation" },
        { "1.2.840.10008.5.1.4.1.1.77.1.6", L"Whole Slide Microscopy Image" },
        { "1.2.840.10008.5.1.4.1.1.88.11", L"Basic Text SR" },
        { "1.2.840.10008.5.1.4.1.1.88.22", L"Enhanced SR" },
        { "1.2.840.10008.5.1.4.1.1.88.33", L"Comprehensive SR" },
        { "1.2.840.10008.5.1.4.1.1.88.59", L"Key Object Selection" },
        { "1.2.840.10008.5.1.4.1.1.88.67", L"X-Ray Radiation Dose SR" },
        { "1.2.840.10008.5.1.4.1.1.104.1", L"Encapsulated PDF" },
        { "1.2.840.10008.5.1.4.1.1.128", L"PET Image" },
        { "1.2.840.10008.5.1.4.1.1.130", L"Enhanced PET Image" },
        { "1.2.840.10008.5.1.4.1.1.481.1", L"RT Image" },
        { "1.2.840.10008.5.1.4.1.1.481.2", L"RT Dose" },
        { "1.2.840.10008.5.1.4.1.1.481.3", L"RT Structure Set" },
        { "1.2.840.10008.5.1.4.1.1.481.5", L"RT Plan" },
    };
    for (auto& n : names) if (uid == n.uid) return n.name;
    std::wstring w;
    for (unsigned char c : uid) w += static_cast<wchar_t>(c);
    return w;
}

// How the pixel data is stored, from the transfer syntax of the meta information
static std::wstring Compression(const std::string& ts)
{
    static const struct { const char* uid; const wchar_t* name; } names[] = {
        { "1.2.840.10008.1.2", L"none" },                       // Implicit VR Little Endian
        { "1.2.840.10008.1.2.1", L"none" },                     // Explicit VR Little Endian
        { "1.2.840.10008.1.2.2", L"none" },                     // Explicit VR Big Endian
        { "1.2.840.10008.1.2.1.99", L"Deflate" },
        { "1.2.840.10008.1.2.5", L"RLE" },
        { "1.2.840.10008.1.2.4.50", L"JPEG baseline" },
        { "1.2.840.10008.1.2.4.51", L"JPEG extended" },
        { "1.2.840.10008.1.2.4.57", L"JPEG lossless" },
        { "1.2.840.10008.1.2.4.70", L"JPEG lossless" },
        { "1.2.840.10008.1.2.4.80", L"JPEG-LS lossless" },
        { "1.2.840.10008.1.2.4.81", L"JPEG-LS near-lossless" },
        { "1.2.840.10008.1.2.4.90", L"JPEG 2000 lossless" },
        { "1.2.840.10008.1.2.4.91", L"JPEG 2000" },
        { "1.2.840.10008.1.2.4.92", L"JPEG 2000 multi-component lossless" },
        { "1.2.840.10008.1.2.4.93", L"JPEG 2000 multi-component" },
        { "1.2.840.10008.1.2.4.100", L"MPEG-2" },
        { "1.2.840.10008.1.2.4.101", L"MPEG-2" },
        { "1.2.840.10008.1.2.4.102", L"MPEG-4" },
        { "1.2.840.10008.1.2.4.103", L"MPEG-4" },
        { "1.2.840.10008.1.2.4.104", L"MPEG-4" },
        { "1.2.840.10008.1.2.4.105", L"MPEG-4" },
        { "1.2.840.10008.1.2.4.106", L"MPEG-4" },
        { "1.2.840.10008.1.2.4.107", L"HEVC" },
        { "1.2.840.10008.1.2.4.108", L"HEVC" },
        { "1.2.840.10008.1.2.4.201", L"HTJ2K lossless" },
        { "1.2.840.10008.1.2.4.202", L"HTJ2K lossless" },
        { "1.2.840.10008.1.2.4.203", L"HTJ2K" },
    };
    if (ts.empty()) return L"none (no meta information)";
    for (auto& n : names) if (ts == n.uid) return n.name;
    std::wstring w;
    for (unsigned char c : ts) w += static_cast<wchar_t>(c);
    return w;
}

static bool Digits(const std::string& s, size_t at, size_t n, int& value)
{
    if (at + n > s.size()) return false;
    value = 0;
    for (size_t i = at; i < at + n; i++)
    {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
    }
    return true;
}

// DICOM date (DA, "YYYYMMDD") and time (TM, "HHMMSS.FFFFFF", maybe shorter, or the old "HH:MM:SS")
// as a FILETIME. DICOM times have no time zone: they are taken as local time, so that Explorer, which
// shows FILETIMEs in local time, shows the same date and time as the header.
static bool FileTimeOf(const std::string& date, std::string time, FILETIME& ft)
{
    SYSTEMTIME st = {};
    int y, mo, d;
    if (date.size() < 8 || !Digits(date, 0, 4, y) || !Digits(date, 4, 2, mo) || !Digits(date, 6, 2, d)) return false;
    if (y < 1601 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    st.wYear = static_cast<WORD>(y); st.wMonth = static_cast<WORD>(mo); st.wDay = static_cast<WORD>(d);
    time.erase(std::remove(time.begin(), time.end(), ':'), time.end());
    int h = 0, mi = 0, s = 0;
    if (!time.empty())
    {
        if (!Digits(time, 0, 2, h)) return false;
        Digits(time, 2, 2, mi);
        Digits(time, 4, 2, s);
        if (time.size() > 7 && time[6] == '.')
        {
            int ms = 0, n = 0;
            for (size_t i = 7; i < time.size() && i < 10 && time[i] >= '0' && time[i] <= '9'; i++, n++) ms = ms * 10 + (time[i] - '0');
            while (n++ < 3) ms *= 10;
            st.wMilliseconds = static_cast<WORD>(ms);
        }
        if (h > 23 || mi > 59 || s > 60) return false;
        st.wHour = static_cast<WORD>(h); st.wMinute = static_cast<WORD>(mi); st.wSecond = static_cast<WORD>(s > 59 ? 59 : s);
    }
    SYSTEMTIME utc;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc)) return false;
    return SystemTimeToFileTime(&utc, &ft) != FALSE;
}

// When the image was acquired: AcquisitionDateTime, else AcquisitionDate + Time, else the content
// (image creation) date and time, else the series ones
static bool Acquired(const Header& h, FILETIME& ft)
{
    if (h.acquisitionDateTime.size() >= 8)
    {
        std::string t = h.acquisitionDateTime.substr(8);
        size_t zone = t.find_first_of("+-");                    // the time zone offset is left out
        if (zone != std::string::npos) t = t.substr(0, zone);
        if (FileTimeOf(h.acquisitionDateTime.substr(0, 8), t, ft)) return true;
    }
    if (FileTimeOf(h.acquisitionDate, h.acquisitionTime, ft)) return true;
    if (FileTimeOf(h.contentDate, h.contentTime, ft)) return true;
    return FileTimeOf(h.seriesDate, h.seriesTime, ft);
}

// The plane of the slice, as slicePlaneName of DICOMstack.m: AX / COR / SAG only within 1e-3 rad of
// a cardinal plane; otherwise the nearest of the cardiac views hla / vla / lvot / sa (nearest neighbour
// against the reference orientations of PlaneClouds.h, with no distance limit). Experimental.
static double AngleBetweenNormals(const double a[3], const double b[3])
{
    double dm = 0, dp = 0;
    for (int i = 0; i < 3; i++) { dm += (a[i] - b[i]) * (a[i] - b[i]); dp += (a[i] + b[i]) * (a[i] + b[i]); }
    auto half = [](double x) { x = sqrt(x) / 2; return asin(x > 1 ? 1 : x); };
    double m = half(dm), p = half(dp);
    return 2 * (m < p ? m : p);
}

template <size_t N>
static double NearestInCloud(const double n[3], const double (&cloud)[N][2])
{
    double best = 1e9;
    for (size_t i = 0; i < N; i++)
    {
        double e = cloud[i][0], a = cloud[i][1];
        double c[3] = { cos(e) * cos(a), cos(e) * sin(a), sin(e) };
        double d = AngleBetweenNormals(n, c);
        if (d < best) best = d;
    }
    return best;
}

static std::wstring PlaneName(const Header& h)
{
    double o[6];
    if (!Numbers(h.imageOrientation, o, 6)) return std::wstring();
    double n[3] = { o[1] * o[5] - o[2] * o[4], o[2] * o[3] - o[0] * o[5], o[0] * o[4] - o[1] * o[3] };
    double norm = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(norm > 0) || !std::isfinite(norm)) return L"unk";
    for (auto& x : n) x /= norm;
    static const double AX[3] = { 0, 0, 1 }, COR[3] = { 0, 1, 0 }, SAG[3] = { 1, 0, 0 };
    if (AngleBetweenNormals(n, AX) < 1e-3) return L"AX";
    if (AngleBetweenNormals(n, COR) < 1e-3) return L"COR";
    if (AngleBetweenNormals(n, SAG) < 1e-3) return L"SAG";
    double d[4] = { NearestInCloud(n, CloudHLA), NearestInCloud(n, CloudVLA), NearestInCloud(n, CloudLVOT), NearestInCloud(n, CloudSA) };
    static const wchar_t* names[4] = { L"hla", L"vla", L"lvot", L"sa" };
    int best = 0;
    for (int i = 1; i < 4; i++) if (d[i] < d[best]) best = i;
    return names[best];
}

// COM object: the properties of a DICOM file, read from its header when it is initialized
class PropertyHandler : public IInitializeWithFile, public IPropertyStore, public IPropertyStoreCapabilities
{
public:
    PropertyHandler() { InterlockedIncrement(&g_locks); }
    virtual ~PropertyHandler()
    {
        if (cache) cache->Release();
        InterlockedDecrement(&g_locks);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IPropertyStore) *ppv = static_cast<IPropertyStore*>(this);
        else if (riid == IID_IInitializeWithFile) *ppv = static_cast<IInitializeWithFile*>(this);
        else if (riid == IID_IPropertyStoreCapabilities) *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithFile: the path is needed (not just a stream) for the duplicates of the file
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD) override
    {
        if (!path) return E_INVALIDARG;
        if (cache) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&cache));
        if (FAILED(hr)) return hr;
        try
        {
            Header h;
            if (ReadHeader(path, h, UpToPixelSpacing) != Kind::Dicom) return S_OK;   // not DICOM: no properties
            if (!h.studyUid.empty()) Set(PKEY_StudyHash, Hash5(h.studyUid));
            if (!h.seriesUid.empty()) Set(PKEY_SeriesHash, Hash5(h.seriesUid));
            Set(PKEY_PatientID, Widen(h.patientId, h.charset));
            Set(PKEY_ImageOrientation, Widen(h.imageOrientation));
            SetInt(PKEY_InstanceNumber, h.instanceNumber);
            if (h.rows) SetUInt(PKEY_Rows, h.rows);
            if (h.columns) SetUInt(PKEY_Columns, h.columns);
            double z;
            if (ZLevel(h, z)) SetDouble(PKEY_ZLevel, z);
            FILETIME ft;
            if (Acquired(h, ft)) SetTime(PKEY_Acquired, ft);
            if (FileTimeOf(h.studyDate, std::string(), ft)) SetTime(PKEY_StudyDate, ft);
            if (!h.frameOfReference.empty()) Set(PKEY_FrameOfReference, Hash5(h.frameOfReference));
            if (!h.sopClass.empty()) Set(PKEY_ObjectType, ObjectType(h.sopClass));
            Set(PKEY_ImageType, Widen(h.imageType));
            SetInt(PKEY_Frames, h.numberOfFrames);
            double number;
            if (Numbers(h.triggerTime, &number, 1)) SetDouble(PKEY_TriggerTime, number);
            if (Numbers(h.sliceThickness, &number, 1)) SetDouble(PKEY_SliceThickness, number);
            Set(PKEY_PixelSpacing, Widen(h.pixelSpacing));
            Set(PKEY_Compression, Compression(h.transferSyntax));
            Set(PKEY_Protocol, Widen(h.protocolName, h.charset));
            Set(PKEY_Manufacturer, Widen(h.manufacturer, h.charset));
            Set(PKEY_Plane, PlaneName(h));
            Set(PKEY_StudyUid, Widen(h.studyUid));
            Set(PKEY_SeriesUid, Widen(h.seriesUid));
            Set(PKEY_SeriesDescription, Widen(h.seriesDescription, h.charset));
            Set(PKEY_Modality, Widen(h.modality));
            SetInt(PKEY_SeriesNumber, h.seriesNumber);
            Set(PKEY_DuplicatedIn, g_duplicates.Of(path));
        }
        catch (...) { }
        return S_OK;
    }

    // IPropertyStore: read only
    IFACEMETHODIMP GetCount(DWORD* count) override { if (!cache) { if (count) *count = 0; return S_OK; } return cache->GetCount(count); }
    IFACEMETHODIMP GetAt(DWORD i, PROPERTYKEY* key) override { return cache ? cache->GetAt(i, key) : E_UNEXPECTED; }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override
    {
        if (!value) return E_POINTER;
        if (!cache) { PropVariantInit(value); return S_OK; }
        return cache->GetValue(key, value);
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Commit() override { return STG_E_ACCESSDENIED; }

    // IPropertyStoreCapabilities: nothing can be edited
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

private:
    LONG refs = 1;
    IPropertyStoreCache* cache = nullptr;

    // IS (integer string) as a number, so that 2 sorts before 10
    void SetInt(REFPROPERTYKEY key, const std::string& text)
    {
        if (text.empty()) return;
        char* end = nullptr;
        long number = strtol(text.c_str(), &end, 10);
        if (!end || *end != 0) return;
        PROPVARIANT v;
        InitPropVariantFromInt32(static_cast<LONG>(number), &v);
        cache->SetValueAndState(key, &v, PSC_NORMAL);
        PropVariantClear(&v);
    }

    void SetDouble(REFPROPERTYKEY key, double number)
    {
        PROPVARIANT v;
        InitPropVariantFromDouble(number, &v);
        cache->SetValueAndState(key, &v, PSC_NORMAL);
        PropVariantClear(&v);
    }

    void SetTime(REFPROPERTYKEY key, const FILETIME& ft)
    {
        PROPVARIANT v;
        InitPropVariantFromFileTime(&ft, &v);
        cache->SetValueAndState(key, &v, PSC_NORMAL);
        PropVariantClear(&v);
    }

    void SetUInt(REFPROPERTYKEY key, unsigned number)
    {
        PROPVARIANT v;
        InitPropVariantFromUInt32(number, &v);
        cache->SetValueAndState(key, &v, PSC_NORMAL);
        PropVariantClear(&v);
    }

    void Set(REFPROPERTYKEY key, const std::wstring& text)
    {
        if (text.empty()) return;
        PROPVARIANT v;
        if (SUCCEEDED(InitPropVariantFromString(text.c_str(), &v)))
        {
            cache->SetValueAndState(key, &v, PSC_NORMAL);
            PropVariantClear(&v);
        }
    }
};

// One factory per class: the icons and the properties
template <class T>
class ClassFactory : public IClassFactory
{
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid != IID_IUnknown && riid != IID_IClassFactory) return E_NOINTERFACE;
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }     // static object
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto obj = new (std::nothrow) T();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks);
        return S_OK;
    }
};

static ClassFactory<IconHandler> g_iconFactory;
static ClassFactory<PropertyHandler> g_propertyFactory;

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid == CLSID_DicomIcons) return g_iconFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_DicomProperties) return g_propertyFactory.QueryInterface(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return g_locks == 0 ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
