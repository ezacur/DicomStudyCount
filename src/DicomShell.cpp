// DicomShell (bin\DicomShell.dll, built by tools\Build.ps1): Explorer icon handler for DICOM files. Every DICOM file gets an icon with its
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
// And the preview (Explorer's preview pane) and the thumbnails of DICOM files, decoded with DCMTK.
//
// Registered by Count-DicomStudies.ps1 -Install for .dcm, .ima, .dicom and files without extension: the
// icons, preview and thumbnails per user (tools\Register-DicomIcons.ps1), the properties for the
// machine (tools\Register-DicomProperties.ps1).
// It runs inside Explorer: every error ends in the generic icon or no properties, never in an exception.

// DCMTK first: its configuration must come before the Windows headers it includes itself
#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcistrmb.h"
#include "dcmtk/dcmdata/dcpxitem.h"
#include "dcmtk/dcmdata/dcrledrg.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#include "dcmtk/dcmimage/diregist.h"
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpls/djdecode.h"
#include "dcmtk/oflog/oflog.h"
#include "openjpeg.h"
#include <memory>
#include <mutex>

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <thumbcache.h>
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
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

// {F4798986-2EEA-49F6-8082-2FC552CE691B}
static const CLSID CLSID_DicomIcons = { 0xF4798986, 0x2EEA, 0x49F6, { 0x80, 0x82, 0x2F, 0xC5, 0x52, 0xCE, 0x69, 0x1B } };
// {21329A2B-09B3-4250-9103-9267877321EB}
static const CLSID CLSID_DicomProperties = { 0x21329A2B, 0x09B3, 0x4250, { 0x91, 0x03, 0x92, 0x67, 0x87, 0x73, 0x21, 0xEB } };
// {F3E184D5-95DF-4BE9-9929-32FF8A59CD55}
static const CLSID CLSID_DicomPreview = { 0xF3E184D5, 0x95DF, 0x4BE9, { 0x99, 0x29, 0x32, 0xFF, 0x8A, 0x59, 0xCD, 0x55 } };
// {F6D7D002-C00D-4893-AC73-DC78D44316C3}
static const CLSID CLSID_DicomThumbnails = { 0xF6D7D002, 0xC00D, 0x4893, { 0xAC, 0x73, 0xDC, 0x78, 0xD4, 0x43, 0x16, 0xC3 } };

static HMODULE g_module = nullptr;

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
    // Identifying data, for the "sensitive data" column (its values are never shown)
    std::string patientName, birthDate, address, phone, otherNames, otherIds, motherName;
    std::string institution, institutionAddress, referring, performing, operators;
    std::string identityRemoved, burnedIn;
};

// Where the reading stops: the icons need the header up to SeriesNumber only, the columns up to
// BurnedInAnnotation (the last attribute they look at)
static const unsigned UpToSeriesNumber = 0x00200011;
static const unsigned UpToBurnedIn = 0x00280301;

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
        // Identifying data
        case 0x00080080: if (!p.String(len, h.institution)) return Kind::Dicom; break;          // InstitutionName
        case 0x00080081: if (!p.String(len, h.institutionAddress)) return Kind::Dicom; break;   // InstitutionAddress
        case 0x00080090: if (!p.String(len, h.referring)) return Kind::Dicom; break;            // ReferringPhysicianName
        case 0x00081050: if (!p.String(len, h.performing)) return Kind::Dicom; break;           // PerformingPhysicianName
        case 0x00081070: if (!p.String(len, h.operators)) return Kind::Dicom; break;            // OperatorsName
        case 0x00100010: if (!p.String(len, h.patientName)) return Kind::Dicom; break;          // PatientName
        case 0x00100030: if (!p.String(len, h.birthDate)) return Kind::Dicom; break;            // PatientBirthDate
        case 0x00101000: if (!p.String(len, h.otherIds)) return Kind::Dicom; break;             // OtherPatientIDs
        case 0x00101001: if (!p.String(len, h.otherNames)) return Kind::Dicom; break;           // OtherPatientNames
        case 0x00101040: if (!p.String(len, h.address)) return Kind::Dicom; break;              // PatientAddress
        case 0x00101060: if (!p.String(len, h.motherName)) return Kind::Dicom; break;           // PatientMotherBirthName
        case 0x00102154: if (!p.String(len, h.phone)) return Kind::Dicom; break;                // PatientTelephoneNumbers
        case 0x00120062: if (!p.String(len, h.identityRemoved)) return Kind::Dicom; break;      // PatientIdentityRemoved
        case 0x00280301: if (!p.String(len, h.burnedIn)) return Kind::Dicom; break;             // BurnedInAnnotation
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

// The square of a series icon (the number in the study color) drawn at (x0, y0): the whole icon, or
// the badge in the corner of a thumbnail
static void DrawBadge(HDC dc, int x0, int y0, int size, int color, const std::wstring& label)
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

// 6 case-sensitive characters (62 possible each) from the 64-bit FNV-1a of a UID: for the
// StudyInstanceUID, the same as the study tags of the folder comments, FolderComments.StudyTag in
// src\DicomStudyCount.cs
static std::wstring Hash6(const std::string& uid)
{
    static const wchar_t Base62[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned long long h = 14695981039346656037ULL;
    for (unsigned char c : uid) { h ^= c; h *= 1099511628211ULL; }
    unsigned long long v = h % 56800235584ULL;              // 62^6
    wchar_t chars[7] = {};
    for (int i = 5; i >= 0; i--) { chars[i] = Base62[v % 62]; v /= 62; }
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
        { "1.2.840.10008.1.2.4.110", L"JPEG XL lossless" },
        { "1.2.840.10008.1.2.4.111", L"JPEG XL (recompressed JPEG)" },
        { "1.2.840.10008.1.2.4.112", L"JPEG XL" },
        { "1.2.840.10008.1.2.8.1", L"Deflated image frames" },
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

static const PROPERTYKEY PKEY_SensitiveData = { FMTID_Dicom, 29 };

// A value that identifies someone: not empty and not a placeholder left by anonymizers
// ("ANONYMOUS", "^", "removed"...)
static bool Identifies(std::string v)
{
    std::string t;
    for (char c : v) if (c != '^' && c != ' ' && c != '*' && c != '=' && c != '-' && c != '\\') t += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (t.empty()) return false;
    static const char* placeholders[] = { "anonymous", "anonymized", "anonymised", "anon", "unknown", "removed", "deidentified",
                                          "none", "na", "x", "xx", "xxx", "patient", "noname", "redacted" };
    for (auto p : placeholders) if (t == p) return false;
    return t.compare(0, 6, "anonym") != 0;
}

// Which kinds of identifying data the header still holds (never the values themselves): "YES: name,
// birth date, institution"; "no"; "de-identified" (PatientIdentityRemoved) with what remains anyway.
// Experimental: it cannot see text burned into the pixels that the header does not declare, nor
// identifying data hidden in private tags.
static std::wstring SensitiveData(const Header& h)
{
    std::wstring found;
    auto add = [&](const std::string& value, const wchar_t* what) {
        if (!Identifies(value)) return;
        if (!found.empty()) found += L", ";
        found += what;
    };
    add(h.patientName, L"name");
    add(h.otherNames, L"other names");
    add(h.motherName, L"mother's name");
    add(h.birthDate, L"birth date");
    add(h.address, L"address");
    add(h.phone, L"phone");
    add(h.otherIds, L"other IDs");
    add(h.referring, L"referring physician");
    add(h.performing, L"performing physician");
    add(h.operators, L"operators");
    add(h.institution, L"institution");
    add(h.institutionAddress, L"institution address");
    if (h.burnedIn == "YES") { if (!found.empty()) found += L", "; found += L"burned-in text"; }
    bool removed = h.identityRemoved == "YES";
    if (found.empty()) return removed ? L"de-identified" : L"no";
    return (removed ? L"de-identified, but: " : L"YES: ") + found;
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
            if (ReadHeader(path, h, UpToBurnedIn) != Kind::Dicom) return S_OK;   // not DICOM: no properties
            if (!h.studyUid.empty()) Set(PKEY_StudyHash, Hash6(h.studyUid));
            if (!h.seriesUid.empty()) Set(PKEY_SeriesHash, Hash6(h.seriesUid));
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
            if (!h.frameOfReference.empty()) Set(PKEY_FrameOfReference, Hash6(h.frameOfReference));
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
            Set(PKEY_SensitiveData, SensitiveData(h));
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

// ---------------------------------------------------------------------------------------------
// Preview (Explorer's preview pane, Alt+P): three tabs (the DICOM columns of Explorer, to show or hide
// them; the header with names; the header with tags) over the image, whose window drags with the
// mouse. It runs in prevhost.exe, a process of its own (a crash there never takes Explorer down), at
// medium integrity (DisableLowILProcessIsolation), to reach the Explorer window for its columns.
// DCMTK reads the file from memory, decodes the pixels (uncompressed, JPEG baseline / extended /
// lossless, JPEG-LS, RLE; JPEG 2000 with OpenJPEG) and renders them with the window of the header.

static void InitDcmtk()
{
    static std::once_flag once;
    std::call_once(once, []() {
        OFLog::configure(OFLogger::OFF_LOG_LEVEL);       // nobody to read its messages here
        // JPEG baseline, extended, lossless. Lossy JPEG: the color space from the JPEG markers, not
        // from the header (some devices store YCbCr and say RGB)
        DJDecoderRegistration::registerCodecs(EDC_guessLossyOnly);
        DJLSDecoderRegistration::registerCodecs();       // JPEG-LS (with the CharLS inside DCMTK)
        DcmRLEDecoderRegistration::registerCodecs();     // RLE
    });
}

// The decoded image of the preview, kept to change its window (brightness / contrast) while the mouse
// drags over it. Monochrome: the window of DCMTK, on the values of the pixels. Color: the same on
// the rendered RGB (0..255), from center 127.5 and width 255 (the image as it is).
struct Imaging
{
    std::unique_ptr<DcmFileFormat> file;
    std::unique_ptr<DcmDataset> decoded;           // JPEG 2000, decoded by OpenJPEG
    std::unique_ptr<DicomImage> image;             // destroyed first: it reads the two above
    bool mono = false;
    std::vector<DWORD> color;
    double center = 127.5, width = 255, center0 = 127.5, width0 = 255, low = 0, high = 255;
};

// What the preview shows: the first frame as 32-bit BGR rows (top-down), a summary line and the
// header twice, one line per element: with the tags "(gggg,eeee)" and with their dictionary names.
// The thumbnail: the frame, and the study and series for its badge.
struct Rendered
{
    int width = 0, height = 0;
    std::vector<DWORD> pixels;
    std::wstring summary, tags, names;
    std::string studyUid, seriesNumber;
    std::shared_ptr<Imaging> imaging;              // only in the preview
};

// The pixels of the frame with the current window of its imaging
static void Rewindow(Rendered& r)
{
    Imaging* im = r.imaging.get();
    if (!im || !im->image || r.width <= 0 || r.height <= 0) return;
    const size_t n = static_cast<size_t>(r.width) * r.height;
    r.pixels.resize(n);
    if (im->width < 1) im->width = 1;
    if (im->mono)
    {
        im->image->setWindow(im->center, im->width);
        const Uint8* out = static_cast<const Uint8*>(im->image->getOutputData(8, 0, 0));
        if (!out) return;
        for (size_t i = 0; i < n; i++) r.pixels[i] = (static_cast<DWORD>(out[i]) << 16) | (out[i] << 8) | out[i];
        return;
    }
    // Color: one table for the three channels
    BYTE table[256];
    const double from = im->center - im->width / 2;
    for (int v = 0; v < 256; v++)
    {
        double x = (v - from) * 255.0 / im->width;
        table[v] = static_cast<BYTE>(x < 0 ? 0 : x > 255 ? 255 : x + 0.5);
    }
    for (size_t i = 0; i < n && i < im->color.size(); i++)
    {
        DWORD c = im->color[i];
        r.pixels[i] = (static_cast<DWORD>(table[(c >> 16) & 0xFF]) << 16) | (table[(c >> 8) & 0xFF] << 8) | table[c & 0xFF];
    }
}

// The value of an element as one line: text and numbers in brackets, large binary values by size
static std::wstring ValueOf(DcmElement* e, const std::string& charset)
{
    const Uint32 length = e->getLength();
    const DcmEVR vr = e->ident();
    if (e->getTag() == DCM_PixelData) return L"(pixel data, " + std::to_wstring(length) + L" bytes)";
    bool binary = vr == EVR_OB || vr == EVR_OW || vr == EVR_OF || vr == EVR_OD || vr == EVR_OL || vr == EVR_OV ||
                  vr == EVR_UN || vr == EVR_ox;
    if ((binary && length > 64) || length > 4096) return L"(" + std::to_wstring(length) + L" bytes)";
    OFString v;
    if (e->getOFStringArray(v).bad()) return L"(not readable)";
    std::wstring w = Widen(std::string(v.c_str(), v.length()), charset);
    for (auto& c : w) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    if (w.size() > 200) w = w.substr(0, 200) + L"...";
    return L"[" + w + L"]";
}

// One line per element; the items of a sequence indented under it
static void Dump(DcmItem* item, const std::string& charset, int depth, std::wstring& tags, std::wstring& names)
{
    const std::wstring indent(static_cast<size_t>(depth) * 2, L' ');
    for (unsigned long i = 0; i < item->card(); i++)
    {
        DcmElement* e = item->getElement(i);
        if (!e) continue;
        DcmTag tag = e->getTag();
        wchar_t numbers[16];
        swprintf_s(numbers, L"(%04X,%04X)", tag.getGTag(), tag.getETag());
        std::wstring name = Widen(tag.getTagName());
        if (name.empty() || name == Widen(DcmTag_ERROR_TagName)) name = numbers;    // not in the dictionary
        if (name.size() < 32) name.resize(32, L' ');
        const std::wstring vr = Widen(DcmVR(e->getVR()).getVRName());
        if (e->ident() == EVR_SQ)
        {
            auto sq = static_cast<DcmSequenceOfItems*>(e);
            std::wstring value = L"(" + std::to_wstring(sq->card()) + (sq->card() == 1 ? L" item)" : L" items)");
            tags += indent + numbers + L" " + vr + L" " + value + L"\r\n";
            names += indent + name + L" " + vr + L" " + value + L"\r\n";
            for (unsigned long k = 0; k < sq->card(); k++)
            {
                std::wstring head = indent + L"  > item " + std::to_wstring(k + 1) + L"\r\n";
                tags += head;
                names += head;
                Dump(sq->getItem(k), charset, depth + 2, tags, names);
            }
            continue;
        }
        const std::wstring value = ValueOf(e, charset);
        tags += indent + numbers + L" " + vr + L" " + value + L"\r\n";
        names += indent + name + L" " + vr + L" " + value + L"\r\n";
    }
}

// JPEG 2000 (and High-Throughput JPEG 2000): DCMTK has no decoder for it, OpenJPEG decodes it
static bool IsJpeg2000(E_TransferSyntax x)
{
    return x == EXS_JPEG2000LosslessOnly || x == EXS_JPEG2000 || x == EXS_JPEG2000MulticomponentLosslessOnly ||
           x == EXS_JPEG2000Multicomponent || x == EXS_HighThroughputJPEG2000LosslessOnly ||
           x == EXS_HighThroughputJPEG2000withRPCLOptionsLosslessOnly || x == EXS_HighThroughputJPEG2000;
}

// OpenJPEG reads the codestream from memory
struct MemoryStream { const Uint8* data; size_t size, at; };
static OPJ_SIZE_T MemRead(void* buffer, OPJ_SIZE_T n, void* user)
{
    auto m = static_cast<MemoryStream*>(user);
    if (m->at >= m->size) return static_cast<OPJ_SIZE_T>(-1);
    size_t k = n < m->size - m->at ? n : m->size - m->at;
    memcpy(buffer, m->data + m->at, k);
    m->at += k;
    return k;
}
static OPJ_OFF_T MemSkip(OPJ_OFF_T n, void* user)
{
    auto m = static_cast<MemoryStream*>(user);
    if (n < 0) return -1;
    size_t k = static_cast<size_t>(n) < m->size - m->at ? static_cast<size_t>(n) : m->size - m->at;
    m->at += k;
    return static_cast<OPJ_OFF_T>(k);
}
static OPJ_BOOL MemSeek(OPJ_OFF_T n, void* user)
{
    auto m = static_cast<MemoryStream*>(user);
    if (n < 0 || static_cast<size_t>(n) > m->size) return OPJ_FALSE;
    m->at = static_cast<size_t>(n);
    return OPJ_TRUE;
}

// The first frame of a JPEG 2000 dataset, decoded: a copy of the dataset with that frame as native
// pixel data (Explicit VR Little Endian), which DicomImage renders as any other. Null if it cannot.
static DcmDataset* DecodeJpeg2000(DcmDataset* ds, bool multiframe)
{
    DcmElement* element = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, element).bad() || !element) return nullptr;
    auto pixels = static_cast<DcmPixelData*>(element);
    E_TransferSyntax key;
    const DcmRepresentationParameter* param = nullptr;
    pixels->getOriginalRepresentationKey(key, param);
    DcmPixelSequence* seq = nullptr;
    if (pixels->getEncapsulatedRepresentation(key, param, seq).bad() || !seq) return nullptr;

    // The fragments of the first frame (item 0 is the offset table): all of them for a single frame;
    // for more, up to the next one that starts a codestream (SOC marker, or a JP2 signature box)
    std::vector<Uint8> code;
    for (unsigned long i = 1; i < seq->card(); i++)
    {
        DcmPixelItem* item = nullptr;
        Uint8* bytes = nullptr;
        if (seq->getItem(item, i).bad() || !item || item->getUint8Array(bytes).bad() || !bytes) break;
        const Uint32 n = item->getLength();
        bool starts = n >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0x4F) || (n >= 8 && bytes[4] == 'j' && bytes[5] == 'P'));
        if (multiframe && !code.empty() && starts) break;
        code.insert(code.end(), bytes, bytes + n);
    }
    if (code.size() < 8) return nullptr;
    bool jp2 = !(code[0] == 0xFF && code[1] == 0x4F);

    MemoryStream mem = { code.data(), code.size(), 0 };
    opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
    if (!stream) return nullptr;
    opj_stream_set_read_function(stream, MemRead);
    opj_stream_set_skip_function(stream, MemSkip);
    opj_stream_set_seek_function(stream, MemSeek);
    opj_stream_set_user_data(stream, &mem, nullptr);
    opj_stream_set_user_data_length(stream, code.size());
    opj_codec_t* codec = opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
    opj_image_t* image = nullptr;
    opj_dparameters_t options;
    opj_set_default_decoder_parameters(&options);
    bool ok = codec && opj_setup_decoder(codec, &options) && opj_read_header(stream, codec, &image) &&
              opj_decode(codec, stream, image) && opj_end_decompress(codec, stream);
    opj_stream_destroy(stream);
    if (codec) opj_destroy_codec(codec);

    DcmDataset* out = nullptr;
    if (ok && image && image->numcomps >= 1)
    {
        const OPJ_UINT32 samples = image->numcomps >= 3 ? 3 : 1;
        const opj_image_comp_t* c = image->comps;
        const OPJ_UINT32 w = c[0].w, h = c[0].h, prec = c[0].prec, sgnd = c[0].sgnd;
        bool same = w > 0 && h > 0 && prec >= 1 && prec <= 16;
        for (OPJ_UINT32 k = 0; k < samples && same; k++)
            same = c[k].w == w && c[k].h == h && c[k].prec == prec && c[k].sgnd == sgnd && c[k].data;
        if (same)
        {
            out = new DcmDataset(*ds);
            out->findAndDeleteElement(DCM_PixelData);
            const size_t n = static_cast<size_t>(w) * h;
            const bool bytes = prec <= 8 && !sgnd;
            if (bytes)
            {
                std::vector<Uint8> v(n * samples);
                for (size_t i = 0; i < n; i++)
                    for (OPJ_UINT32 k = 0; k < samples; k++) v[i * samples + k] = static_cast<Uint8>(c[k].data[i]);
                out->putAndInsertUint8Array(DCM_PixelData, v.data(), static_cast<unsigned long>(v.size()));
            }
            else
            {
                std::vector<Uint16> v(n * samples);
                for (size_t i = 0; i < n; i++)
                    for (OPJ_UINT32 k = 0; k < samples; k++) v[i * samples + k] = static_cast<Uint16>(c[k].data[i]);
                out->putAndInsertUint16Array(DCM_PixelData, v.data(), static_cast<unsigned long>(v.size()));
            }
            out->putAndInsertUint16(DCM_SamplesPerPixel, static_cast<Uint16>(samples));
            out->putAndInsertUint16(DCM_Rows, static_cast<Uint16>(h));
            out->putAndInsertUint16(DCM_Columns, static_cast<Uint16>(w));
            out->putAndInsertUint16(DCM_BitsAllocated, bytes ? 8 : 16);
            out->putAndInsertUint16(DCM_BitsStored, static_cast<Uint16>(prec));
            out->putAndInsertUint16(DCM_HighBit, static_cast<Uint16>(prec - 1));
            out->putAndInsertUint16(DCM_PixelRepresentation, sgnd ? 1 : 0);
            out->findAndDeleteElement(DCM_NumberOfFrames);
            if (samples == 3)
            {
                // OpenJPEG undoes the color transform of the codestream: RGB
                out->putAndInsertString(DCM_PhotometricInterpretation, "RGB");
                out->putAndInsertUint16(DCM_PlanarConfiguration, 0);
            }
            else
            {
                OFString photometric;
                out->findAndGetOFString(DCM_PhotometricInterpretation, photometric);
                if (photometric != "MONOCHROME1") out->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
                out->findAndDeleteElement(DCM_PlanarConfiguration);
            }
        }
    }
    if (image) opj_image_destroy(image);
    return out;
}

static Rendered RenderDicom(const std::vector<char>& data, bool header = true)
{
    Rendered r;
    InitDcmtk();
    DcmInputBufferStream in;
    in.setBuffer(data.data(), static_cast<offile_off_t>(data.size()));
    in.setEos();
    auto im = std::make_shared<Imaging>();
    im->file = std::make_unique<DcmFileFormat>();
    DcmFileFormat& ff = *im->file;
    ff.transferInit();
    OFCondition st = ff.read(in, EXS_Unknown, EGL_noChange, DCM_MaxReadLength);
    ff.transferEnd();
    if (st.bad())
    {
        // Deflated files: this DCMTK has no zlib (as the one of I3Dfileformat)
        const char deflate[] = "1.2.840.10008.1.2.1.99";
        bool deflated = std::search(data.begin(), data.begin() + (data.size() < 4096 ? data.size() : 4096),
                                    deflate, deflate + sizeof(deflate) - 1) != data.begin() + (data.size() < 4096 ? data.size() : 4096);
        r.summary = deflated ? L"Deflated DICOM: not shown by this preview" : L"Not readable as DICOM: " + Widen(st.text());
        return r;
    }
    DcmDataset* ds = ff.getDataset();

    OFString charset, sopClass, tsUid;
    ds->findAndGetOFString(DCM_SpecificCharacterSet, charset);
    ds->findAndGetOFString(DCM_SOPClassUID, sopClass);
    const E_TransferSyntax xfer = ds->getOriginalXfer();
    tsUid = DcmXfer(xfer).getXferID();
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;

    OFString studyUid, seriesNumber;
    ds->findAndGetOFString(DCM_StudyInstanceUID, studyUid);
    ds->findAndGetOFString(DCM_SeriesNumber, seriesNumber);
    r.studyUid = studyUid.c_str();
    r.seriesNumber = seriesNumber.c_str();

    if (header)
    {
        const std::wstring meta = L"# File meta information\r\n", dataset = L"\r\n# Dataset\r\n";
        r.tags = meta;
        r.names = meta;
        Dump(ff.getMetaInfo(), std::string(), 0, r.tags, r.names);
        r.tags += dataset;
        r.names += dataset;
        Dump(ds, charset.c_str(), 0, r.tags, r.names);
    }

    r.summary = sopClass.empty() ? L"DICOM" : ObjectType(sopClass.c_str());
    if (ds->tagExists(DCM_PixelData))
    {
        // Only the first frame is decoded: a multi-frame file can be large. JPEG 2000 by OpenJPEG first.
        auto& decoded = im->decoded;
        if (IsJpeg2000(xfer)) decoded.reset(DecodeJpeg2000(ds, frames > 1));
        im->image = std::make_unique<DicomImage>(decoded ? static_cast<DcmObject*>(decoded.get()) : &ff,
                                                 decoded ? EXS_LittleEndianExplicit : xfer, CIF_UsePartialAccessToPixelData, 0, 1);
        DicomImage& image = *im->image;
        if (image.getStatus() == EIS_Normal)
        {
            r.width = static_cast<int>(image.getWidth());
            r.height = static_cast<int>(image.getHeight());
            im->mono = image.isMonochrome() != 0;
            if (im->mono)
            {
                // The window of the header, or the range of the pixels; the range bounds the dragging
                if (image.getWindowCount() > 0) image.setWindow(0);
                else image.setMinMaxWindow();
                image.getMinMaxValues(im->low, im->high);
                if (!image.getWindow(im->center, im->width) || im->width < 1)
                {
                    im->center = (im->low + im->high) / 2;
                    im->width = im->high - im->low + 1;
                }
                im->center0 = im->center;
                im->width0 = im->width;
                r.imaging = im;
                Rewindow(r);
            }
            else
            {
                const Uint8* out = static_cast<const Uint8*>(image.getOutputData(8, 0, 0));
                if (out && r.width > 0 && r.height > 0)
                {
                    const size_t n = static_cast<size_t>(r.width) * r.height;
                    r.pixels.resize(n);
                    for (size_t i = 0; i < n; i++)
                        r.pixels[i] = (static_cast<DWORD>(out[3 * i]) << 16) | (out[3 * i + 1] << 8) | out[3 * i + 2];
                    if (header) im->color = r.pixels;
                    r.imaging = im;
                }
            }
            if (!header) r.imaging.reset();          // the thumbnail: only its pixels
            r.summary += L"  -  " + std::to_wstring(r.width) + L" x " + std::to_wstring(r.height);
            if (frames > 1) r.summary += L"  -  frame 1 of " + std::to_wstring(frames);
        }
        else
        {
            // Compressed with something there is no decoder for here (JPEG 2000, HTJ2K, MPEG, HEVC...)
            DcmXfer x(xfer);
            bool decodable = !x.isEncapsulated() || xfer == EXS_RLELossless || xfer == EXS_JPEGLSLossless ||
                             xfer == EXS_JPEGLSLossy || x.getJPEGProcess8Bit() != 0;
            if (IsJpeg2000(xfer) && !decoded) r.summary += L"  -  image not shown: its JPEG 2000 data could not be decoded";
            else r.summary += decodable || decoded ? L"  -  image not shown: " + Widen(DicomImage::getString(image.getStatus()))
                                                   : L"  -  image not shown: " + Compression(tsUid.c_str()) + L" is not supported";
        }
    }
    else r.summary += L"  -  no image";
    r.summary += L"  -  compression: " + Compression(tsUid.c_str());
    return r;
}

// The whole file in memory, up to 1 GB
static HRESULT ReadAll(IStream* stream, std::vector<char>& data)
{
    STATSTG stat = {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) stat.cbSize.QuadPart = 0;
    if (stat.cbSize.QuadPart > (1ULL << 30)) return E_OUTOFMEMORY;
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    data.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    size_t got = 0;
    while (true)
    {
        if (got == data.size()) data.resize(got + 1024 * 1024);
        ULONG n = 0;
        HRESULT hr = stream->Read(data.data() + got, static_cast<ULONG>(data.size() - got), &n);
        if (FAILED(hr)) return hr;
        got += n;
        if (n == 0 || hr == S_FALSE) break;
    }
    data.resize(got);
    return S_OK;
}

// The DICOM columns, as the tab "Columns" of the preview lists them: the order of Properties > Details
static const PROPERTYKEY* const g_columnKeys[] = {
    &PKEY_StudyHash, &PKEY_SeriesHash, &PKEY_Modality, &PKEY_SeriesNumber, &PKEY_InstanceNumber, &PKEY_SeriesDescription,
    &PKEY_Protocol, &PKEY_Plane, &PKEY_Acquired, &PKEY_TriggerTime, &PKEY_ZLevel, &PKEY_Rows, &PKEY_Columns,
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
static HANDLE ThemedControls()
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

class PreviewHandler : public IInitializeWithItem, public IPreviewHandler, public IPreviewHandlerVisuals,
                       public IOleWindow, public IObjectWithSite
{
public:
    PreviewHandler() { InterlockedIncrement(&g_locks); }
    virtual ~PreviewHandler()
    {
        Unload();
        if (site) site->Release();
        InterlockedDecrement(&g_locks);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
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
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithItem (not a stream): the path is needed, for the folder (its tab, its Explorer
    // window) and for the values of the columns. The host asks for a stream first, so there is none.
    IFACEMETHODIMP Initialize(IShellItem* i, DWORD) override
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
    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* rc) override
    {
        if (!hwnd || !rc) return E_INVALIDARG;
        parent = hwnd;
        bounds = *rc;
        if (wnd)
        {
            ::SetParent(wnd, parent);
            Place();
        }
        return S_OK;
    }
    IFACEMETHODIMP SetRect(const RECT* rc) override
    {
        if (!rc) return E_INVALIDARG;
        bounds = *rc;
        if (wnd) Place();
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() override
    {
        if (wnd) return S_OK;
        if (!item || !parent) return E_UNEXPECTED;
        try
        {
            IStream* stream = nullptr;
            HRESULT hr = item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&stream));
            if (FAILED(hr)) return hr;
            std::vector<char> data;
            hr = ReadAll(stream, data);
            stream->Release();
            if (FAILED(hr)) return hr;
            view = RenderDicom(data);
        }
        catch (...) { view = Rendered(); view.summary = L"This file cannot be shown"; }
        return CreateWindows();
    }
    IFACEMETHODIMP Unload() override
    {
        if (wnd) { DestroyWindow(wnd); wnd = nullptr; edit = nullptr; list = nullptr; }
        for (HFONT* f : { &font, &uiFont, &boldFont }) if (*f) { DeleteObject(*f); *f = nullptr; }
        if (background) { DeleteObject(background); background = nullptr; }
        if (item) { item->Release(); item = nullptr; }
        path.clear();
        folder.clear();
        note.clear();
        view = Rendered();
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override
    {
        HWND content = Content();
        if (!content) return S_FALSE;
        ::SetFocus(content);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* focus) override
    {
        if (!focus) return E_INVALIDARG;
        *focus = GetFocus();
        return *focus ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override
    {
        if (!msg) return E_INVALIDARG;
        // In the header: Ctrl+A selects it all, Ctrl+C (or Ctrl+Insert) copies the selection; in the
        // columns the same with rows. Ctrl+T: between the two headers. Explorer would take Ctrl+C to
        // copy the file.
        HWND focus = GetFocus();
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
    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override
    {
        backColor = color;
        if (background) { DeleteObject(background); background = nullptr; }
        Recolor();
        return S_OK;
    }
    IFACEMETHODIMP SetFont(const LOGFONTW*) override { return S_OK; }
    IFACEMETHODIMP SetTextColor(COLORREF color) override
    {
        textColor = color;
        Recolor();
        return S_OK;
    }

    // IOleWindow
    IFACEMETHODIMP GetWindow(HWND* hwnd) override
    {
        if (!hwnd) return E_INVALIDARG;
        *hwnd = parent;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* s) override
    {
        if (site) site->Release();
        site = s;
        if (site) site->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return site ? site->QueryInterface(riid, ppv) : E_FAIL;
    }

private:
    enum { TabColumns, TabHeader, TabTags, TabCount };
    enum class Drag { None, Split, Window };
    static constexpr UINT WM_READCOLUMNS = WM_APP + 1;
    static constexpr const wchar_t* Key = L"Software\\DicomStudyCount\\Preview";

    LONG refs = 1;
    IShellItem* item = nullptr;
    IUnknown* site = nullptr;
    std::wstring path, folder, note;
    HWND parent = nullptr, wnd = nullptr, edit = nullptr, list = nullptr;
    RECT bounds = {};
    RECT tabArea = {}, tabRects[TabCount] = {}, contentArea = {}, splitArea = {}, imageArea = {}, statusArea = {};
    int tab = TabColumns;
    int split = 450;                                // the share of the top panel, in thousandths
    Drag drag = Drag::None;
    POINT dragFrom = {};
    double dragCenter = 0, dragWidth = 0;
    bool filling = false;                           // the checks change from the code, not from a click
    Rendered view;
    HFONT font = nullptr, uiFont = nullptr, boldFont = nullptr;
    HBRUSH background = nullptr;
    COLORREF backColor = GetSysColor(COLOR_WINDOW), textColor = GetSysColor(COLOR_WINDOWTEXT);

    HBRUSH Background()
    {
        if (!background) background = CreateSolidBrush(backColor);
        return background;
    }

    // Between the background and the text: lines and the inactive tabs
    COLORREF Dim(int percent) const
    {
        auto mix = [&](int shift) {
            return ((((textColor >> shift) & 0xFF) * percent + ((backColor >> shift) & 0xFF) * (100 - percent)) / 100) << shift;
        };
        return static_cast<COLORREF>(mix(0) | mix(8) | mix(16));
    }

    int Px(int n) const
    {
        UINT dpi = wnd ? GetDpiForWindow(wnd) : 96;
        return MulDiv(n, dpi ? dpi : 96, 96);
    }

    HWND Content() const { return tab == TabColumns ? list : edit; }

    static DWORD ReadDword(const wchar_t* key, const wchar_t* name, DWORD fallback)
    {
        DWORD value = 0, size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
    }

    HRESULT CreateWindows()
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
        if (!folder.empty())
            tab = static_cast<int>(ReadDword(L"Software\\DicomStudyCount\\Preview\\Tabs", folder.c_str(), TabColumns));
        if (tab < 0 || tab >= TabCount) tab = TabColumns;
        split = static_cast<int>(ReadDword(Key, L"Split", 450));
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
        // The controls of the common controls 6 (themed), not the old ones prevhost.exe would give
        ULONG_PTR cookie = 0;
        HANDLE context = ThemedControls();
        bool active = context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie);
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);
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
        Recolor();
        Place();
        ShowContent();
        // Which columns Explorer shows: after DoPreview has returned, Explorer is not waiting for it
        if (list) PostMessageW(wnd, WM_READCOLUMNS, 0, 0);
        return S_OK;
    }

    // One row per DICOM column: its name and its value for this file, checked if the Explorer window
    // of the folder shows it (WM_READCOLUMNS)
    void FillColumns()
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

        IPropertyStore* values = nullptr;
        if (!path.empty())
        {
            auto handler = new (std::nothrow) PropertyHandler();
            if (handler)
            {
                if (SUCCEEDED(handler->Initialize(path.c_str(), STGM_READ))) handler->QueryInterface(IID_PPV_ARGS(&values));
                handler->Release();
            }
        }
        filling = true;
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(g_columnKeys)); i++)
        {
            const PROPERTYKEY& key = *g_columnKeys[i];
            std::wstring name, value;
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
            LVITEMW row = {};
            row.mask = LVIF_TEXT | LVIF_PARAM;
            row.iItem = i;
            row.pszText = const_cast<wchar_t*>(name.c_str());
            row.lParam = i;
            int at = ListView_InsertItem(list, &row);
            ListView_SetItemText(list, at, 1, const_cast<wchar_t*>(value.c_str()));
        }
        filling = false;
        if (values) values->Release();
        ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
    }

    // The checks: the columns the Explorer window of the folder shows
    void ReadColumns()
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
    void Toggle(int row)
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
    void CopyRows()
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

    // The tab chosen, remembered for this folder (HKCU\Software\DicomStudyCount\Preview\Tabs)
    void SelectTab(int t)
    {
        if (t == tab || t < 0 || t >= TabCount) return;
        const bool headers = (t == TabHeader || t == TabTags) && (tab == TabHeader || tab == TabTags);
        LRESULT first = headers ? SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0) : 0;
        tab = t;
        if (edit && t != TabColumns)
        {
            SetWindowTextW(edit, (t == TabTags ? view.tags : view.names).c_str());
            if (headers) SendMessageW(edit, EM_LINESCROLL, 0, first);   // both have the same lines
        }
        if (!folder.empty())
        {
            DWORD value = static_cast<DWORD>(t);
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\DicomStudyCount\\Preview\\Tabs", folder.c_str(), REG_DWORD,
                            &value, sizeof(value));
        }
        ShowContent();
        if (HWND content = Content()) ::SetFocus(content);
        InvalidateRect(wnd, &tabArea, TRUE);
        InvalidateRect(wnd, &statusArea, TRUE);
    }

    void ShowContent()
    {
        if (list) ShowWindow(list, tab == TabColumns ? SW_SHOWNA : SW_HIDE);
        if (edit) ShowWindow(edit, tab == TabColumns ? SW_HIDE : SW_SHOWNA);
    }

    void Recolor()
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
        if (wnd) InvalidateRect(wnd, nullptr, TRUE);
    }

    void Place()
    {
        SetWindowPos(wnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
    }

    // From the top: the tabs, their content, the bar that moves the split, the image, the status line
    // (the summary, and the window of the image). Without an image the content takes its place.
    void Layout()
    {
        RECT c;
        GetClientRect(wnd, &c);
        const int w = c.right, h = c.bottom, line = Px(22), bar = Px(6);
        tabArea = { 0, 0, w, line + Px(4) };
        statusArea = { Px(4), h - line, w - Px(4), h };
        const int top = tabArea.bottom, bottom = h - line > top ? h - line : top;
        if (!view.pixels.empty())
        {
            int y = top + static_cast<int>(static_cast<long long>(bottom - top - bar) * split / 1000);
            if (y > bottom - bar - line) y = bottom - bar - line;
            if (y < top + line) y = top + line;
            contentArea = { 0, top, w, y };
            splitArea = { 0, y, w, y + bar };
            imageArea = { 0, y + bar, w, bottom };
        }
        else
        {
            contentArea = { 0, top, w, bottom };
            splitArea = imageArea = {};
        }
        const int ch = contentArea.bottom - contentArea.top > 0 ? contentArea.bottom - contentArea.top : 0;
        if (edit) MoveWindow(edit, 0, contentArea.top, w, ch, TRUE);
        if (list) MoveWindow(list, 0, contentArea.top, w, ch, TRUE);
        InvalidateRect(wnd, nullptr, TRUE);
    }

    void Paint(HDC dc)
    {
        RECT c;
        GetClientRect(wnd, &c);
        FillRect(dc, &c, Background());
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));

        // The tabs: the chosen one in bold and underlined, the others dimmed
        static const wchar_t* const labels[TabCount] = { L"Columns", L"Header", L"Tags" };
        int x = Px(6);
        for (int t = 0; t < TabCount; t++)
        {
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
        FillRect(dc, &under, line);
        if (splitArea.bottom > splitArea.top)
        {
            const int y = (splitArea.top + splitArea.bottom) / 2;
            RECT bar = { 0, y, c.right, y + Px(1) };
            FillRect(dc, &bar, line);
        }
        DeleteObject(line);

        if (!view.pixels.empty())
        {
            // Fitted into the image area, keeping the aspect ratio, centered
            int aw = imageArea.right - imageArea.left, ah = imageArea.bottom - imageArea.top;
            int dw = aw, dh = static_cast<int>(static_cast<long long>(aw) * view.height / view.width);
            if (dh > ah) { dh = ah; dw = static_cast<int>(static_cast<long long>(ah) * view.width / view.height); }
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = view.width;
            bi.bmiHeader.biHeight = -view.height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, imageArea.left + (aw - dw) / 2, imageArea.top + (ah - dh) / 2, dw, dh, 0, 0, view.width, view.height,
                          view.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }

        // The status line: the summary (or a note), and at its right the window of the image
        SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
        RECT text = statusArea;
        if (Imaging* im = view.imaging.get())
        {
            wchar_t window[96];
            if (im->mono) swprintf_s(window, L"Center %.0f   Width %.0f", im->center, im->width);
            else swprintf_s(window, L"Brightness %+.0f   Contrast %.0f%%", 127.5 - im->center, 25500.0 / im->width);
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

    // Dragging over the image: right widens the window (less contrast), up lowers its center (brighter).
    // The whole range of the pixels in the height of the image area.
    void MoveWindowTo(POINT pt)
    {
        Imaging* im = view.imaging.get();
        if (!im) return;
        const int area = imageArea.bottom - imageArea.top > Px(100) ? imageArea.bottom - imageArea.top : Px(100);
        const double step = (im->high - im->low > 1 ? im->high - im->low : 1) / area;
        im->width = dragWidth + (pt.x - dragFrom.x) * step;
        im->center = dragCenter + (pt.y - dragFrom.y) * step;
        if (im->width < 1) im->width = 1;
        Redraw();
    }

    void Redraw()
    {
        Rewindow(view);
        InvalidateRect(wnd, &imageArea, FALSE);
        InvalidateRect(wnd, &statusArea, FALSE);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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
        case WM_SETCURSOR:
            if (self && LOWORD(lp) == HTCLIENT)
            {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                for (const RECT& r : self->tabRects) if (PtInRect(&r, pt)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
                if (PtInRect(&self->splitArea, pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE; }
                if (self->view.imaging && PtInRect(&self->imageArea, pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return TRUE; }
            }
            break;
        case WM_LBUTTONDOWN:
            if (self)
            {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                for (int t = 0; t < TabCount; t++) if (PtInRect(&self->tabRects[t], pt)) { self->SelectTab(t); return 0; }
                if (PtInRect(&self->splitArea, pt)) self->drag = Drag::Split;
                else if (self->view.imaging && PtInRect(&self->imageArea, pt))
                {
                    self->drag = Drag::Window;
                    self->dragCenter = self->view.imaging->center;
                    self->dragWidth = self->view.imaging->width;
                }
                if (self->drag != Drag::None) { self->dragFrom = pt; SetCapture(hwnd); }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (self && self->drag != Drag::None)
            {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                if (self->drag == Drag::Window) self->MoveWindowTo(pt);
                else
                {
                    const int top = self->tabArea.bottom, room = self->statusArea.top - top - (self->splitArea.bottom - self->splitArea.top);
                    if (room > 0)
                    {
                        int s = static_cast<int>(static_cast<long long>(pt.y - top) * 1000 / room);
                        self->split = s < 0 ? 0 : s > 1000 ? 1000 : s;
                        self->Layout();
                    }
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (self && self->drag != Drag::None)
            {
                if (self->drag == Drag::Split)
                {
                    DWORD value = static_cast<DWORD>(self->split);
                    RegSetKeyValueW(HKEY_CURRENT_USER, Key, L"Split", REG_DWORD, &value, sizeof(value));
                }
                self->drag = Drag::None;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (self) self->drag = Drag::None;
            return 0;
        case WM_LBUTTONDBLCLK:
            // On the image: its first window again
            if (self && self->view.imaging)
            {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                if (PtInRect(&self->imageArea, pt))
                {
                    self->view.imaging->center = self->view.imaging->center0;
                    self->view.imaging->width = self->view.imaging->width0;
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
};

// Thumbnails (Explorer's medium icons and larger): the first frame, with the series icon as a badge in
// its lower right corner. DICOM without an image (reports, DICOMDIR...) fails, and keeps its icon.
class ThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    ThumbnailProvider() { InterlockedIncrement(&g_locks); }
    virtual ~ThumbnailProvider()
    {
        if (stream) stream->Release();
        InterlockedDecrement(&g_locks);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
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

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* s, DWORD) override
    {
        if (!s) return E_INVALIDARG;
        if (stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream = s;
        stream->AddRef();
        return S_OK;
    }

    // IThumbnailProvider: at most cx x cx, keeping the aspect ratio
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override
    {
        if (!bitmap || !alpha) return E_POINTER;
        *bitmap = nullptr;
        if (!stream || cx == 0) return E_UNEXPECTED;
        try
        {
            std::vector<char> data;
            HRESULT hr = ReadAll(stream, data);
            if (FAILED(hr)) return hr;
            Rendered r = RenderDicom(data, false);
            if (r.pixels.empty()) return E_FAIL;
            int w = static_cast<int>(cx), h = static_cast<int>(cx);
            if (r.width >= r.height) h = static_cast<int>(static_cast<long long>(cx) * r.height / r.width);
            else w = static_cast<int>(static_cast<long long>(cx) * r.width / r.height);
            if (w < 1) w = 1;
            if (h < 1) h = 1;

            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            if (!dc) return E_FAIL;
            HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib) { DeleteDC(dc); return E_OUTOFMEMORY; }
            HGDIOBJ old = SelectObject(dc, dib);

            BITMAPINFO src = bi;
            src.bmiHeader.biWidth = r.width;
            src.bmiHeader.biHeight = -r.height;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, 0, 0, w, h, 0, 0, r.width, r.height, r.pixels.data(), &src, DIB_RGB_COLORS, SRCCOPY);

            // The badge: a quarter of the thumbnail size, up to half the shorter side (narrow images),
            // from 16 px; none on tiny thumbnails
            int badge = static_cast<int>(cx) / 4;
            if (badge > (w < h ? w : h) / 2) badge = (w < h ? w : h) / 2;
            if (badge >= 16)
            {
                int color = r.studyUid.empty() ? NoStudyColor : ColorIndex(r.studyUid);
                std::wstring label;
                for (char c : r.seriesNumber) if (label.size() < 6 && c != ' ') label += static_cast<wchar_t>(static_cast<unsigned char>(c));
                if (label.empty()) label = L"?";
                DrawBadge(dc, w - badge, h - badge, badge, color, label);
            }
            GdiFlush();
            auto px = static_cast<DWORD*>(bits);
            for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) px[i] |= 0xFF000000;
            SelectObject(dc, old);
            DeleteDC(dc);
            *bitmap = dib;
            *alpha = WTSAT_RGB;
            return S_OK;
        }
        catch (...) { return E_FAIL; }
    }

private:
    LONG refs = 1;
    IStream* stream = nullptr;
};

// One factory per class: the icons, the properties, the preview and the thumbnails
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
static ClassFactory<PreviewHandler> g_previewFactory;
static ClassFactory<ThumbnailProvider> g_thumbnailFactory;

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid == CLSID_DicomIcons) return g_iconFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_DicomProperties) return g_propertyFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_DicomPreview) return g_previewFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_DicomThumbnails) return g_thumbnailFactory.QueryInterface(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return g_locks == 0 ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_module = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
