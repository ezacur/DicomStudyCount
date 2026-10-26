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
// And the DICOM explorer: a window of its own, run by rundll32 (ExploreW), with the DICOM files of a
// folder as a tree of patients, studies, series, orientations, positions and images.
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
#include "dcmtk/dcmimgle/dipixel.h"
#include "dcmtk/dcmimage/diregist.h"
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpls/djdecode.h"
#include "dcmtk/oflog/oflog.h"
#include "openjpeg.h"
#include "zlib.h"
#include <memory>
#include <mutex>

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <thumbcache.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <new>
#include <numeric>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "normaliz.lib")

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/EXPORT:ExploreW")              // rundll32 DicomShell.dll,Explore <folder>

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

// ---------------------------------------------------------------------------------------------
// Chaining: the handlers another program had for an extension (files without one included) before the
// install put these in front of them (tools\Register-DicomIcons.ps1 in HKCU\Software\DicomStudyCount\
// Chain\<ext>, Register-DicomProperties.ps1 in HKLM\SOFTWARE\DicomStudyCount\PropertyHandler\Chain\<ext>).
// Files that are not DICOM go to them, as if this DLL were not there.

static const wchar_t ChainKey[] = L"Software\\DicomStudyCount\\Chain";
static const wchar_t PropertyChainKey[] = L"SOFTWARE\\DicomStudyCount\\PropertyHandler\\Chain";

// ".ima" (lowercase), or "(none)"
static std::wstring ChainName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/:"), dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) || dot + 1 == path.size()) return L"(none)";
    std::wstring ext = path.substr(dot);
    CharLowerBuffW(&ext[0], static_cast<DWORD>(ext.size()));
    return ext;
}

// What was there before for the extension of `path`: a CLSID, or the location of an icon
static std::wstring Previous(HKEY root, const wchar_t* base, const std::wstring& path, const wchar_t* name)
{
    const std::wstring key = std::wstring(base) + L"\\" + ChainName(path);
    wchar_t value[1024];
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return std::wstring();
    return value;
}

// The previous handler, created in this process; null if there is none, or if it cannot be (a 32-bit one)
template <class I>
static I* CreatePrevious(const std::wstring& clsidText)
{
    CLSID clsid;
    if (clsidText.empty() || FAILED(CLSIDFromString(clsidText.c_str(), &clsid))) return nullptr;
    for (const CLSID* mine : { &CLSID_DicomIcons, &CLSID_DicomProperties, &CLSID_DicomPreview, &CLSID_DicomThumbnails })
        if (clsid == *mine) return nullptr;
    I* p = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(I), reinterpret_cast<void**>(&p)))) return nullptr;
    return p;
}

// The first bytes of a file: "DICM" after the preamble, or a "raw" DICOM (group 0002 or 0008 first),
// as ReadHeader tries them
static bool LooksDicom(const unsigned char* b, size_t n)
{
    if (n >= 132 && b[128] == 'D' && b[129] == 'I' && b[130] == 'C' && b[131] == 'M') return true;
    return n >= 8 && b[1] == 0 && (b[0] == 0x02 || b[0] == 0x08);
}

static bool StreamLooksDicom(IStream* stream)
{
    unsigned char head[132] = {};
    LARGE_INTEGER zero = {};
    ULONG got = 0, n = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    while (got < sizeof(head) && SUCCEEDED(stream->Read(head + got, sizeof(head) - got, &n)) && n > 0) got += n;
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    return LooksDicom(head, got);
}

// The name of the file of a stream (for its extension), empty if it has none
static std::wstring StreamName(IStream* stream)
{
    STATSTG stat = {};
    std::wstring name;
    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_DEFAULT)) && stat.pwcsName)
    {
        name = stat.pwcsName;
        CoTaskMemFree(stat.pwcsName);
    }
    return name;
}

class IconHandler : public IPersistFile, public IExtractIconW
{
public:
    IconHandler() { InterlockedIncrement(&g_locks); }
    virtual ~IconHandler()
    {
        if (previous) previous->Release();
        InterlockedDecrement(&g_locks);
    }

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
                // Not DICOM: the icon it had before the install (its handler, or its fixed icon)
                if (!path.empty())
                {
                    if (!previous && (previous = CreatePrevious<IExtractIconW>(Previous(HKEY_CURRENT_USER, ChainKey, path, L"IconHandler"))))
                    {
                        IPersistFile* load = nullptr;
                        HRESULT hr = previous->QueryInterface(IID_PPV_ARGS(&load));
                        if (SUCCEEDED(hr)) { hr = load->Load(path.c_str(), STGM_READ); load->Release(); }
                        if (FAILED(hr)) { previous->Release(); previous = nullptr; }
                    }
                    if (previous) return previous->GetIconLocation(flags, file, cch, index, outFlags);
                    if (FixedIcon(Previous(HKEY_CURRENT_USER, ChainKey, path, L"Icon"), file, cch, index))
                    {
                        *outFlags = 0;
                        return S_OK;
                    }
                }
                // Nothing before: the generic file icon
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

    IFACEMETHODIMP Extract(LPCWSTR file, UINT index, HICON* largeIcon, HICON* smallIcon, UINT sizes) override
    {
        if (!file) return E_INVALIDARG;
        try
        {
            std::wstring key = file;
            size_t prefix = wcslen(KeyPrefix);
            if (key.compare(0, prefix, KeyPrefix) != 0)
            {
                // The icon of the handler from before; another one (generic, fixed): Explorer extracts it
                if (previous) return previous->Extract(file, index, largeIcon, smallIcon, sizes);
                return S_FALSE;
            }
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
    IExtractIconW* previous = nullptr;              // the icon handler from before, for a file that is not DICOM

    // A fixed icon from before ("C:\...\imageres.dll,-5205"), not "%1" (each file its own icon)
    static bool FixedIcon(const std::wstring& location, LPWSTR file, UINT cch, int* index)
    {
        if (location.empty() || location.find(L"%1") != std::wstring::npos) return false;
        std::wstring where = location;
        int number = 0;
        const size_t comma = where.find_last_of(L',');
        if (comma != std::wstring::npos)
        {
            number = _wtoi(where.c_str() + comma + 1);
            where.resize(comma);
        }
        if (where.size() >= 2 && where.front() == L'"' && where.back() == L'"') where = where.substr(1, where.size() - 2);
        wchar_t expanded[MAX_PATH];
        const DWORD n = ExpandEnvironmentStringsW(where.c_str(), expanded, MAX_PATH);
        if (n == 0 || n > MAX_PATH || wcslen(expanded) + 1 > cch) return false;
        wcscpy_s(file, cch, expanded);
        *index = number;
        return true;
    }
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
        { "1.3.46.670589.33.1.4.1", L"Philips PMSCT_RLE1 (private)" },
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
        if (previous) previous->Release();
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
    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD mode) override
    {
        if (!path) return E_INVALIDARG;
        if (cache || previous) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&cache));
        if (FAILED(hr)) return hr;
        try
        {
            Header h;
            if (ReadHeader(path, h, UpToBurnedIn) != Kind::Dicom)
            {
                // Not DICOM: the properties of the handler from before, if there was one
                InitializePrevious(path, mode);
                return S_OK;
            }
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

    // IPropertyStore: read only (those of the handler from before, as it does them)
    IFACEMETHODIMP GetCount(DWORD* count) override
    {
        if (previous) return previous->GetCount(count);
        if (!cache) { if (count) *count = 0; return S_OK; }
        return cache->GetCount(count);
    }
    IFACEMETHODIMP GetAt(DWORD i, PROPERTYKEY* key) override
    {
        if (previous) return previous->GetAt(i, key);
        return cache ? cache->GetAt(i, key) : E_UNEXPECTED;
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override
    {
        if (previous) return previous->GetValue(key, value);
        if (!value) return E_POINTER;
        if (!cache) { PropVariantInit(value); return S_OK; }
        return cache->GetValue(key, value);
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT value) override
    {
        return previous ? previous->SetValue(key, value) : STG_E_ACCESSDENIED;
    }
    IFACEMETHODIMP Commit() override { return previous ? previous->Commit() : STG_E_ACCESSDENIED; }

    // IPropertyStoreCapabilities: nothing can be edited (in a DICOM file)
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY key) override
    {
        if (!previous) return S_FALSE;
        IPropertyStoreCapabilities* c = nullptr;
        if (FAILED(previous->QueryInterface(IID_PPV_ARGS(&c)))) return S_OK;     // without it: all of them
        const HRESULT hr = c->IsPropertyWritable(key);
        c->Release();
        return hr;
    }

private:
    LONG refs = 1;
    IPropertyStoreCache* cache = nullptr;
    IPropertyStore* previous = nullptr;             // the handler from before, for a file that is not DICOM

    // It may take the path, or a stream
    void InitializePrevious(LPCWSTR path, DWORD mode)
    {
        IUnknown* p = CreatePrevious<IUnknown>(Previous(HKEY_LOCAL_MACHINE, PropertyChainKey, path, L"Properties"));
        if (!p) return;
        HRESULT hr = E_NOINTERFACE;
        IInitializeWithFile* withFile = nullptr;
        IInitializeWithStream* withStream = nullptr;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&withFile))))
        {
            hr = withFile->Initialize(path, mode);
            withFile->Release();
        }
        else if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&withStream))))
        {
            IStream* s = nullptr;
            const DWORD access = (mode & (STGM_WRITE | STGM_READWRITE)) ? STGM_READWRITE | STGM_SHARE_DENY_WRITE : STGM_READ | STGM_SHARE_DENY_NONE;
            hr = SHCreateStreamOnFileEx(path, access, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &s);
            if (SUCCEEDED(hr)) { hr = withStream->Initialize(s, mode); s->Release(); }
            withStream->Release();
        }
        if (SUCCEEDED(hr)) p->QueryInterface(IID_PPV_ARGS(&previous));
        p->Release();
    }

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

// The decoded file of the preview, kept to change its frame, and its window (brightness / contrast)
// while the mouse drags over it. Monochrome: the window of DCMTK, on the values of the pixels. Color:
// the same on the rendered RGB (0..255), from center 127.5 and width 255 (the image as it is).
struct Imaging
{
    std::unique_ptr<DcmFileFormat> file;
    std::unique_ptr<DcmDataset> decoded;           // JPEG 2000, decoded by OpenJPEG
    std::unique_ptr<DicomImage> image;             // destroyed first: it reads the two above
    E_TransferSyntax xfer = EXS_Unknown;
    bool mono = false;
    std::vector<DWORD> color;
    double center = 127.5, width = 255, center0 = 127.5, width0 = 255, low = 0, high = 255;
};

// A record of a DICOMDIR (patient, study, series, image...): its line in the tree, the file it refers
// to (relative to the DICOMDIR) and the records under it
struct DirNode
{
    std::string type;
    std::wstring text, file;
    long number = 0;                               // a series: its number; a file: its instance number
    std::vector<DirNode> children;
};

// What the preview shows: one frame as 32-bit BGR rows (top-down), a summary line, the header twice
// (one line per element: with the tags "(gggg,eeee)" and with their dictionary names) and the text of
// the overlay. The thumbnail: the first frame, and the study and series for its badge.
struct Rendered
{
    int width = 0, height = 0;
    std::vector<DWORD> pixels;
    std::wstring summary, tags, names;
    std::string studyUid, seriesNumber;
    int frames = 1, frame = 0;
    double frameTime = 0;                          // ms from frame to frame (the cine), 0 if not known
    std::wstring corners[3];                       // the overlay: top left, top right, bottom left (lines)
    std::wstring sides[4];                         // the patient directions at the left, top, right, bottom
    std::vector<DirNode> directory;                // a DICOMDIR: its patients
    std::shared_ptr<Imaging> imaging;              // only in the preview (the file, even without image)
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

// The header twice: with the tags and with the dictionary names
static void DumpHeader(DcmFileFormat& ff, std::wstring& tags, std::wstring& names)
{
    OFString charset;
    ff.getDataset()->findAndGetOFString(DCM_SpecificCharacterSet, charset);
    const std::wstring meta = L"# File meta information\r\n", dataset = L"\r\n# Dataset\r\n";
    tags = meta;
    names = meta;
    Dump(ff.getMetaInfo(), std::string(), 0, tags, names);
    tags += dataset;
    names += dataset;
    Dump(ff.getDataset(), charset.c_str(), 0, tags, names);
}

// "20230405" and "101233.5" as "2023-04-05 10:12:33"
static std::wstring DateTimeText(const std::string& date, const std::string& time)
{
    std::string s;
    if (date.size() >= 8) s = date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2);
    if (time.size() >= 4)
    {
        if (!s.empty()) s += " ";
        s += time.substr(0, 2) + ":" + time.substr(2, 2);
        if (time.size() >= 6 && time[4] >= '0' && time[4] <= '9') s += ":" + time.substr(4, 2);
    }
    return Widen(s);
}

// The patient directions (DICOM: x to the left, y to the back, z to the head) of a direction of the
// image: its main components, the largest first
static std::wstring Direction(double x, double y, double z)
{
    struct Axis { double v; wchar_t plus, minus; } axes[3] = { { x, L'L', L'R' }, { y, L'P', L'A' }, { z, L'H', L'F' } };
    std::sort(std::begin(axes), std::end(axes), [](const Axis& a, const Axis& b) { return std::fabs(a.v) > std::fabs(b.v); });
    std::wstring s;
    for (const Axis& a : axes) if (std::fabs(a.v) >= 0.25) s += a.v > 0 ? a.plus : a.minus;
    return s;
}

// The overlay: what viewers write in the corners of the image, never who the patient is. The series
// and the image at the top left, when and with what at the top right, where the slice is and its size
// at the bottom left. The window and the zoom (bottom right) change, so the preview writes them.
static void Annotate(Rendered& r, DcmDataset* ds)
{
    OFString cs;
    ds->findAndGetOFString(DCM_SpecificCharacterSet, cs);
    const std::string charset = cs.c_str();
    auto get = [&](const DcmTagKey& key, bool deep = false) {
        OFString v;
        ds->findAndGetOFStringArray(key, v, deep);       // deep: in the functional groups of enhanced files
        return std::string(v.c_str(), v.length());
    };
    auto add = [](std::wstring& corner, const std::wstring& line) {
        if (!line.empty()) corner += (corner.empty() ? L"" : L"\n") + line;
    };
    wchar_t buffer[128];

    std::wstring series = Widen(get(DCM_Modality));
    const std::string number = get(DCM_SeriesNumber);
    if (!number.empty()) series += (series.empty() ? L"Series " : L"  Series ") + Widen(number);
    add(r.corners[0], series);
    add(r.corners[0], Widen(get(DCM_SeriesDescription), charset));
    const std::string instance = get(DCM_InstanceNumber);
    if (!instance.empty()) add(r.corners[0], L"Image " + Widen(instance));

    const std::string at = get(DCM_AcquisitionDateTime);
    std::wstring when = at.size() >= 8 ? DateTimeText(at.substr(0, 8), at.substr(8)) : std::wstring();
    if (when.empty()) when = DateTimeText(get(DCM_AcquisitionDate), get(DCM_AcquisitionTime));
    if (when.empty()) when = DateTimeText(get(DCM_ContentDate), get(DCM_ContentTime));
    if (when.empty()) when = DateTimeText(get(DCM_SeriesDate), get(DCM_SeriesTime));
    if (when.empty()) when = DateTimeText(get(DCM_StudyDate), get(DCM_StudyTime));
    add(r.corners[1], when);
    std::wstring device = Widen(get(DCM_Manufacturer), charset);
    const std::wstring model = Widen(get(DCM_ManufacturerModelName), charset);
    if (!model.empty()) device += (device.empty() ? L"" : L" ") + model;
    add(r.corners[1], device);
    add(r.corners[1], Widen(get(DCM_ProtocolName), charset));

    Header h;
    h.imagePosition = get(DCM_ImagePositionPatient, true);
    h.imageOrientation = get(DCM_ImageOrientationPatient, true);
    double z = 0, thickness = 0, spacing[2] = {};
    if (ZLevel(h, z) || Numbers(get(DCM_SliceLocation), &z, 1))
    {
        swprintf_s(buffer, L"Z %.1f mm", z);
        add(r.corners[2], buffer);
    }
    if (Numbers(get(DCM_SliceThickness, true), &thickness, 1))
    {
        swprintf_s(buffer, L"Thickness %.3g mm", thickness);
        add(r.corners[2], buffer);
    }
    if (Numbers(get(DCM_PixelSpacing, true), spacing, 2))
    {
        swprintf_s(buffer, L"Pixel %.3g x %.3g mm", spacing[0], spacing[1]);
        add(r.corners[2], buffer);
    }
    if (r.width > 0)
    {
        swprintf_s(buffer, L"%d x %d", r.width, r.height);
        add(r.corners[2], buffer);
    }

    // The rows go to the right along the first 3 numbers of ImageOrientationPatient, the columns down
    // along the other 3
    double o[6];
    if (Numbers(h.imageOrientation, o, 6))
    {
        r.sides[0] = Direction(-o[0], -o[1], -o[2]);
        r.sides[1] = Direction(-o[3], -o[4], -o[5]);
        r.sides[2] = Direction(o[0], o[1], o[2]);
        r.sides[3] = Direction(o[3], o[4], o[5]);
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

// A frame of a JPEG 2000 dataset, decoded: a copy of the dataset with that frame as native pixel data
// (Explicit VR Little Endian), which DicomImage renders as any other. Null if it cannot.
static DcmDataset* DecodeJpeg2000(DcmDataset* ds, bool multiframe, int frame)
{
    DcmElement* element = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, element).bad() || !element) return nullptr;
    auto pixels = static_cast<DcmPixelData*>(element);
    E_TransferSyntax key;
    const DcmRepresentationParameter* param = nullptr;
    pixels->getOriginalRepresentationKey(key, param);
    DcmPixelSequence* seq = nullptr;
    if (pixels->getEncapsulatedRepresentation(key, param, seq).bad() || !seq) return nullptr;

    // The fragments of the frame (item 0 is the offset table): all of them for a single frame; for more,
    // a frame starts at each fragment that starts a codestream (SOC marker, or a JP2 signature box)
    std::vector<Uint8> code;
    int at = -1;                                    // the frame of the fragment
    for (unsigned long i = 1; i < seq->card(); i++)
    {
        DcmPixelItem* item = nullptr;
        Uint8* bytes = nullptr;
        if (seq->getItem(item, i).bad() || !item || item->getUint8Array(bytes).bad() || !bytes) break;
        const Uint32 n = item->getLength();
        bool starts = n >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0x4F) || (n >= 8 && bytes[4] == 'j' && bytes[5] == 'P'));
        if (multiframe)
        {
            if (at < 0 || starts) at++;
            if (at > frame) break;
            if (at < frame) continue;
        }
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

// Elscint / Philips PMSCT_RLE1 (transfer syntax 1.3.46.670589.33.1.4.1): some Elscint CT (later Picker,
// Marconi, Philips) write the image into the private (07A1,100A), with the name of its codec in
// (07A1,1011), and no (7FE0,0010). DCMTK has no decoder for it.
static const char PmsctSyntax[] = "1.3.46.670589.33.1.4.1";

static bool IsPmsct(DcmFileFormat& ff)
{
    OFString syntax, codec;
    ff.getMetaInfo()->findAndGetOFString(DCM_TransferSyntaxUID, syntax);
    ff.getDataset()->findAndGetOFString(DcmTagKey(0x07A1, 0x1011), codec);
    // (some say so in their transfer syntax, others only by having no pixel data)
    return codec == "PMSCT_RLE1" && (syntax == PmsctSyntax || !ff.getDataset()->tagExists(DCM_PixelData));
}

// Its pixels, as pmsct_decode.cpp (mTools) decodes them: a run-length pass (0xA5, count - 1, byte) and
// then a delta pass (each byte a signed step from the value before; 0x5A, low, high: a value). A copy
// of the dataset with them as native 16-bit pixel data; null if the stream is short or broken.
static DcmDataset* DecodePmsct(DcmDataset* ds)
{
    DcmElement* e = nullptr;
    if (ds->findAndGetElement(DcmTagKey(0x07A1, 0x100A), e).bad() || !e) return nullptr;
    const Uint32 length = e->getLength();
    std::vector<Uint8> in(length);
    Uint8* bytes = nullptr;
    Uint16* words = nullptr;
    if (e->getUint8Array(bytes).good() && bytes) memcpy(in.data(), bytes, length);
    else if (e->getUint16Array(words).good() && words) memcpy(in.data(), words, length);
    else return nullptr;
    std::vector<Uint8> runs;
    runs.reserve(static_cast<size_t>(length) * 2);
    for (size_t i = 0; i < in.size(); i++)
    {
        if (in[i] != 0xA5) { runs.push_back(in[i]); continue; }
        if (i + 2 >= in.size()) return nullptr;
        runs.insert(runs.end(), static_cast<size_t>(in[i + 1]) + 1, in[i + 2]);
        i += 2;
    }
    std::vector<Uint16> out;
    out.reserve(runs.size());
    Uint16 value = 0;
    for (size_t i = 0; i < runs.size(); i++)
    {
        if (runs[i] == 0x5A)
        {
            if (i + 2 >= runs.size()) return nullptr;
            value = static_cast<Uint16>(runs[i + 2] * 256 + runs[i + 1]);
            i += 2;
        }
        else value = static_cast<Uint16>(static_cast<int>(static_cast<Sint8>(runs[i])) + value);
        out.push_back(value);
    }
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    const size_t n = static_cast<size_t>(rows) * columns;
    if (n == 0 || out.size() < n) return nullptr;
    auto copy = new DcmDataset(*ds);
    copy->findAndDeleteElement(DcmTagKey(0x07A1, 0x100A));
    copy->findAndDeleteElement(DCM_PixelData);
    copy->putAndInsertUint16Array(DCM_PixelData, out.data(), static_cast<unsigned long>(n));
    copy->putAndInsertUint16(DCM_BitsAllocated, 16);
    Uint16 stored = 0;
    if (copy->findAndGetUint16(DCM_BitsStored, stored).bad() || stored == 0 || stored > 16) copy->putAndInsertUint16(DCM_BitsStored, 16);
    copy->findAndGetUint16(DCM_BitsStored, stored);
    copy->putAndInsertUint16(DCM_HighBit, static_cast<Uint16>(stored - 1));
    copy->putAndInsertUint16(DCM_SamplesPerPixel, 1);
    OFString photometric;
    if (copy->findAndGetOFString(DCM_PhotometricInterpretation, photometric).bad() || photometric.empty())
        copy->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
    copy->findAndDeleteElement(DCM_NumberOfFrames);
    return copy;
}

// The fragments of one frame of encapsulated pixel data (item 0 is the offset table): all of them for
// a single frame; for more, one fragment a frame when there are as many, or else the offset table
static bool FrameBytes(DcmDataset* ds, bool multiframe, int frame, std::vector<Uint8>& out)
{
    out.clear();
    DcmElement* element = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, element).bad() || !element) return false;
    auto pixels = static_cast<DcmPixelData*>(element);
    E_TransferSyntax key;
    const DcmRepresentationParameter* param = nullptr;
    pixels->getOriginalRepresentationKey(key, param);
    DcmPixelSequence* seq = nullptr;
    if (pixels->getEncapsulatedRepresentation(key, param, seq).bad() || !seq || seq->card() < 2) return false;
    const unsigned long fragments = seq->card() - 1;
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;
    auto append = [&](unsigned long i) {
        DcmPixelItem* item = nullptr;
        Uint8* bytes = nullptr;
        if (seq->getItem(item, i).bad() || !item || item->getUint8Array(bytes).bad() || !bytes) return false;
        out.insert(out.end(), bytes, bytes + item->getLength());
        return true;
    };
    if (!multiframe || frames == 1)
    {
        for (unsigned long i = 1; i <= fragments; i++) if (!append(i)) return false;
        return !out.empty();
    }
    if (static_cast<long>(fragments) == frames) return append(static_cast<unsigned long>(frame) + 1);
    // The offset table: where each frame starts, counted from the first fragment (8 bytes of header each)
    DcmPixelItem* table = nullptr;
    Uint8* offsets = nullptr;
    if (seq->getItem(table, 0).bad() || !table || table->getLength() < static_cast<Uint32>(4 * frames) || table->getUint8Array(offsets).bad() || !offsets)
        return false;
    auto at = [&](int k) { Uint32 v; memcpy(&v, offsets + 4 * k, 4); return v; };
    const Uint32 from = at(frame), to = frame + 1 < frames ? at(frame + 1) : 0xFFFFFFFF;
    Uint32 position = 0;
    for (unsigned long i = 1; i <= fragments; i++)
    {
        DcmPixelItem* item = nullptr;
        if (seq->getItem(item, i).bad() || !item) return false;
        if (position >= from && position < to && !append(i)) return false;
        position += item->getLength() + 8;
    }
    return !out.empty();
}

// A copy of a dataset with one frame as native pixel data (Explicit VR Little Endian), as the decoders
// below give it to DicomImage: 8 or 16 bits as stored, no other frames
static DcmDataset* NativeCopy(DcmDataset* ds, const std::vector<Uint8>& bytes, Uint16 allocated)
{
    auto out = new DcmDataset(*ds);
    out->findAndDeleteElement(DCM_PixelData);
    out->findAndDeleteElement(DCM_FloatPixelData);
    out->findAndDeleteElement(DCM_DoubleFloatPixelData);
    if (allocated <= 8) out->putAndInsertUint8Array(DCM_PixelData, bytes.data(), static_cast<unsigned long>(bytes.size()));
    else
    {
        std::vector<Uint16> words(bytes.size() / 2);
        memcpy(words.data(), bytes.data(), words.size() * 2);
        out->putAndInsertUint16Array(DCM_PixelData, words.data(), static_cast<unsigned long>(words.size()));
    }
    out->findAndDeleteElement(DCM_NumberOfFrames);
    return out;
}

// Deflated image frames (1.2.840.10008.1.2.8.1): each frame compressed by deflate (raw, or with a zlib
// header), which DCMTK reads the name of but has no codec for. 1-bit frames (segmentations) come out as
// 8 bits, 0 or 255. Null if it cannot.
static DcmDataset* DecodeDeflatedFrame(DcmDataset* ds, bool multiframe, int frame)
{
    std::vector<Uint8> packed;
    if (!FrameBytes(ds, multiframe, frame, packed)) return nullptr;
    Uint16 rows = 0, columns = 0, allocated = 0, samples = 1;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    ds->findAndGetUint16(DCM_SamplesPerPixel, samples);
    const size_t n = static_cast<size_t>(rows) * columns * (samples ? samples : 1);
    if (n == 0 || (allocated != 1 && allocated != 8 && allocated != 16)) return nullptr;
    const size_t size = allocated == 1 ? (n + 7) / 8 : n * allocated / 8;
    std::vector<Uint8> plain(size);
    // raw deflate first (-15), then with a zlib or gzip header (47)
    bool ok = false;
    for (int bits : { -15, 47 })
    {
        z_stream z = {};
        if (inflateInit2(&z, bits) != Z_OK) continue;
        z.next_in = packed.data();
        z.avail_in = static_cast<uInt>(packed.size());
        z.next_out = plain.data();
        z.avail_out = static_cast<uInt>(plain.size());
        const int r = inflate(&z, Z_FINISH);
        ok = (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) && z.avail_out == 0;
        inflateEnd(&z);
        if (ok) break;
    }
    if (!ok) return nullptr;
    if (allocated == 1)
    {
        // (bits from the lowest one of each byte, as DICOM packs them)
        std::vector<Uint8> wide(n);
        for (size_t i = 0; i < n; i++) wide[i] = (plain[i / 8] >> (i % 8)) & 1 ? 255 : 0;
        DcmDataset* out = NativeCopy(ds, wide, 8);
        out->putAndInsertUint16(DCM_BitsAllocated, 8);
        out->putAndInsertUint16(DCM_BitsStored, 8);
        out->putAndInsertUint16(DCM_HighBit, 7);
        out->putAndInsertUint16(DCM_PixelRepresentation, 0);
        return out;
    }
    return NativeCopy(ds, plain, allocated);
}

// 1-bit pixels as they are (segmentations): a frame need not start at a byte, and DCMTK reads only the
// frames that do. Its bits, from the lowest one of each byte, as 8 bits (0 or 255).
static bool IsBitFrames(DcmDataset* ds, E_TransferSyntax xfer)
{
    Uint16 allocated = 0;
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    return allocated == 1 && !DcmXfer(xfer).isEncapsulated() && ds->tagExists(DCM_PixelData);
}

static DcmDataset* DecodeBitFrame(DcmDataset* ds, int frame)
{
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    const size_t n = static_cast<size_t>(rows) * columns, first = static_cast<size_t>(frame) * n;
    DcmElement* e = nullptr;
    if (n == 0 || ds->findAndGetElement(DCM_PixelData, e).bad() || !e) return nullptr;
    Uint8* bytes = nullptr;
    Uint16* words = nullptr;                        // (OW: the same bytes, little endian)
    if (e->getUint8Array(bytes).bad() || !bytes)
    {
        if (e->getUint16Array(words).bad() || !words) return nullptr;
        bytes = reinterpret_cast<Uint8*>(words);
    }
    const unsigned long count = e->getLength();
    if ((first + n + 7) / 8 > count) return nullptr;
    std::vector<Uint8> wide(n);
    for (size_t i = 0; i < n; i++) wide[i] = (bytes[(first + i) / 8] >> ((first + i) % 8)) & 1 ? 255 : 0;
    DcmDataset* out = NativeCopy(ds, wide, 8);
    out->putAndInsertUint16(DCM_BitsAllocated, 8);
    out->putAndInsertUint16(DCM_BitsStored, 8);
    out->putAndInsertUint16(DCM_HighBit, 7);
    out->putAndInsertUint16(DCM_PixelRepresentation, 0);
    return out;
}

// Float pixel data (Parametric Maps: T1, T2, ADC... as decimal numbers, 7FE0,0008 or 7FE0,0009): the
// values of a frame, which DCMTK cannot render
static bool FloatFrame(DcmDataset* ds, int frame, std::vector<double>& values)
{
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    const size_t n = static_cast<size_t>(rows) * columns, at = static_cast<size_t>(frame) * n;
    if (n == 0) return false;
    DcmElement* e = nullptr;
    if (ds->findAndGetElement(DCM_FloatPixelData, e).good() && e)
    {
        Float32* f = nullptr;
        if (e->getFloat32Array(f).bad() || !f || e->getLength() / 4 < at + n) return false;
        values.assign(f + at, f + at + n);
        return true;
    }
    if (ds->findAndGetElement(DCM_DoubleFloatPixelData, e).good() && e)
    {
        Float64* d = nullptr;
        if (e->getFloat64Array(d).bad() || !d || e->getLength() / 8 < at + n) return false;
        values.assign(d + at, d + at + n);
        return true;
    }
    return false;
}

// For the preview: the frame as 16-bit values with a rescale back to the decimal ones, so DicomImage
// shows them and their window is in their units
static DcmDataset* DecodeFloatFrame(DcmDataset* ds, int frame)
{
    std::vector<double> values;
    if (!FloatFrame(ds, frame, values)) return nullptr;
    double lo = 0, hi = 0;
    bool first = true;
    for (double v : values)
        if (std::isfinite(v)) { if (first) { lo = hi = v; first = false; } else { lo = (std::min)(lo, v); hi = (std::max)(hi, v); } }
    const double slope = hi > lo ? (hi - lo) / 65535 : 1;
    std::vector<Uint8> bytes(values.size() * 2);
    for (size_t i = 0; i < values.size(); i++)
    {
        const double v = std::isfinite(values[i]) ? values[i] : lo;
        const Uint16 s = static_cast<Uint16>(std::lround((v - lo) / slope));
        memcpy(&bytes[2 * i], &s, 2);
    }
    DcmDataset* out = NativeCopy(ds, bytes, 16);
    out->putAndInsertUint16(DCM_BitsAllocated, 16);
    out->putAndInsertUint16(DCM_BitsStored, 16);
    out->putAndInsertUint16(DCM_HighBit, 15);
    out->putAndInsertUint16(DCM_PixelRepresentation, 0);
    out->putAndInsertUint16(DCM_SamplesPerPixel, 1);
    out->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
    char text[64];
    sprintf_s(text, "%.10g", slope);
    out->putAndInsertString(DCM_RescaleSlope, text);
    sprintf_s(text, "%.10g", lo);
    out->putAndInsertString(DCM_RescaleIntercept, text);
    return out;
}

static bool HasFloatPixels(DcmDataset* ds) { return ds->tagExists(DCM_FloatPixelData) || ds->tagExists(DCM_DoubleFloatPixelData); }

// Why a file has no image to show: it has no pixels (a report, a PDF, a presentation state...), or
// they are in a transfer syntax that is not known here. Empty if it may have one.
static std::wstring NoImage(DcmFileFormat& ff)
{
    OFString syntax;
    ff.getMetaInfo()->findAndGetOFString(DCM_TransferSyntaxUID, syntax);
    if (IsPmsct(ff)) return std::wstring();
    if (!syntax.empty() && DcmXfer(syntax.c_str()).getXfer() == EXS_Unknown) return Compression(syntax.c_str()) + L" is not supported";
    if (!ff.getDataset()->tagExists(DCM_PixelData) && !HasFloatPixels(ff.getDataset()))
    {
        OFString sop;
        ff.getDataset()->findAndGetOFString(DCM_SOPClassUID, sop);
        return L"it has no pixels (" + (sop.empty() ? std::wstring(L"no SOP class") : ObjectType(sop.c_str())) + L")";
    }
    return std::wstring();
}

// The records of a DICOMDIR, as a tree: each one points to the next one of its level and to the first
// one under it by their offsets in the file, which DCMTK keeps for each record it reads. (Not by
// DcmDicomDir, which writes the file back when it finds something to correct in it.) Never who the
// patient is: the patient ID, not the name.
static void ReadRecords(const std::unordered_map<Uint32, DcmItem*>& records, Uint32 offset, const std::string& charset,
                        std::vector<DirNode>& out, int depth, std::unordered_set<Uint32>& seen)
{
    for (; offset != 0 && depth < 8; )
    {
        auto it = records.find(offset);
        if (it == records.end() || !seen.insert(offset).second) return;
        DcmItem* rec = it->second;
        Uint32 next = 0, lower = 0;
        Uint16 inUse = 0xFFFF;
        rec->findAndGetUint32(DCM_OffsetOfTheNextDirectoryRecord, next);
        rec->findAndGetUint32(DCM_OffsetOfReferencedLowerLevelDirectoryEntity, lower);
        rec->findAndGetUint16(DCM_RecordInUseFlag, inUse);
        offset = next;
        if (inUse == 0) continue;                   // deleted
        auto get = [&](const DcmTagKey& key) {
            OFString v;
            rec->findAndGetOFStringArray(key, v);
            return std::string(v.c_str(), v.length());
        };
        DirNode n;
        n.type = get(DCM_DirectoryRecordType);
        n.file = Widen(get(DCM_ReferencedFileID));
        n.number = atol(get(n.type == "SERIES" ? DCM_SeriesNumber : DCM_InstanceNumber).c_str());
        std::wstring text;
        auto add = [&](const std::wstring& s) { if (!s.empty()) text += (text.empty() ? L"" : L"   ") + s; };
        if (n.type == "PATIENT")
        {
            const std::string id = get(DCM_PatientID);
            add(id.empty() ? L"Patient" : L"Patient " + Widen(id, charset));
        }
        else if (n.type == "STUDY")
        {
            add(DateTimeText(get(DCM_StudyDate), get(DCM_StudyTime)));
            add(Widen(get(DCM_StudyDescription), charset));
            const std::string uid = get(DCM_StudyInstanceUID);
            if (!uid.empty()) add(L"[" + Hash6(uid) + L"]");
            if (text.empty()) text = L"Study";
        }
        else if (n.type == "SERIES")
        {
            add(Widen(get(DCM_Modality)));
            const std::string number = get(DCM_SeriesNumber);
            add(number.empty() ? L"Series" : L"Series " + Widen(number));
            add(Widen(get(DCM_SeriesDescription), charset));
        }
        else
        {
            // IMAGE, SR DOCUMENT, PRESENTATION... by their number
            std::string type = n.type.empty() ? "Record" : n.type;
            for (size_t i = 1; i < type.size(); i++) if (type[i - 1] != ' ') type[i] = static_cast<char>(tolower(static_cast<unsigned char>(type[i])));
            const std::string number = get(DCM_InstanceNumber);
            add(Widen(type) + (number.empty() ? L"" : L" " + Widen(number)));
        }
        n.text = text;
        ReadRecords(records, lower, charset, n.children, depth + 1, seen);
        // How many there are under it; the images by their number
        if (!n.children.empty())
        {
            const size_t k = n.children.size();
            const wchar_t* what = n.type == "PATIENT" ? (k == 1 ? L"study" : L"studies") :
                                  n.type == "STUDY" ? L"series" : (k == 1 ? L"file" : L"files");
            n.text += L"   (" + std::to_wstring(k) + L" " + what + L")";
            // The series of a study by their number, the files of a series by theirs
            if (n.type == "STUDY" || std::all_of(n.children.begin(), n.children.end(), [](const DirNode& c) { return c.children.empty(); }))
                std::stable_sort(n.children.begin(), n.children.end(), [](const DirNode& a, const DirNode& b) { return a.number < b.number; });
        }
        out.push_back(std::move(n));
    }
}

static std::vector<DirNode> ReadDirectory(DcmDataset* ds)
{
    std::vector<DirNode> roots;
    DcmSequenceOfItems* seq = nullptr;
    if (ds->findAndGetSequence(DCM_DirectoryRecordSequence, seq).bad() || !seq) return roots;
    std::unordered_map<Uint32, DcmItem*> records;
    for (unsigned long i = 0; i < seq->card(); i++)
    {
        DcmItem* item = seq->getItem(i);
        if (item && item->ident() == EVR_dirRecord) records[static_cast<DcmDirectoryRecord*>(item)->getFileOffset()] = item;
    }
    Uint32 first = 0;
    ds->findAndGetUint32(DCM_OffsetOfTheFirstDirectoryRecordOfTheRootDirectoryEntity, first);
    OFString charset;
    ds->findAndGetOFString(DCM_SpecificCharacterSet, charset);
    std::unordered_set<Uint32> seen;
    ReadRecords(records, first, charset.c_str(), roots, 0, seen);
    return roots;
}

// One frame of the file into the pixels, by a DicomImage of its own (a JPEG 2000 one decoded first by
// OpenJPEG). Its window: the one of the header, or the range of the pixels; `keep`: the one in use, as
// the frames change. An empty string if it is shown; if not, why (and no pixels).
static std::wstring ShowFrame(Rendered& r, int frame, bool keep)
{
    Imaging* im = r.imaging.get();
    if (!im || !im->file) return L"no image";
    const double center = im->center, width = im->width;
    const bool mono = im->mono;
    im->image.reset();
    im->decoded.reset();
    r.pixels.clear();
    r.width = r.height = 0;
    DcmFileFormat& ff = *im->file;
    auto& decoded = im->decoded;
    const std::wstring none = NoImage(ff);
    if (!none.empty()) return none;
    if (IsJpeg2000(im->xfer))
    {
        decoded.reset(DecodeJpeg2000(ff.getDataset(), r.frames > 1, frame));
        if (!decoded) return L"its JPEG 2000 data could not be decoded";
    }
    else if (IsPmsct(ff))
    {
        decoded.reset(DecodePmsct(ff.getDataset()));
        if (!decoded) return L"its PMSCT_RLE1 data could not be decoded";
    }
    else if (IsBitFrames(ff.getDataset(), im->xfer))
    {
        decoded.reset(DecodeBitFrame(ff.getDataset(), frame));
        if (!decoded) return L"its 1-bit pixels could not be read";
    }
    else if (im->xfer == EXS_DeflatedImageFrameCompression)
    {
        decoded.reset(DecodeDeflatedFrame(ff.getDataset(), r.frames > 1, frame));
        if (!decoded) return L"its deflated image frames could not be decoded";
    }
    else if (HasFloatPixels(ff.getDataset()))
    {
        decoded.reset(DecodeFloatFrame(ff.getDataset(), frame));
        if (!decoded) return L"its floating point pixels could not be read";
    }
    im->image = std::make_unique<DicomImage>(decoded ? static_cast<DcmObject*>(decoded.get()) : &ff,
                                             decoded ? EXS_LittleEndianExplicit : im->xfer, CIF_UsePartialAccessToPixelData,
                                             decoded ? 0 : static_cast<unsigned long>(frame), 1);
    DicomImage& image = *im->image;
    if (image.getStatus() != EIS_Normal)
    {
        // Compressed with something there is no decoder for here (MPEG, HEVC...)
        DcmXfer x(im->xfer);
        bool decodable = !x.isEncapsulated() || im->xfer == EXS_RLELossless || im->xfer == EXS_JPEGLSLossless ||
                         im->xfer == EXS_JPEGLSLossy || x.getJPEGProcess8Bit() != 0 || decoded;
        std::wstring why = decodable ? Widen(DicomImage::getString(image.getStatus()))
                                     : Compression(x.getXferID()) + L" is not supported";
        im->image.reset();
        decoded.reset();
        return why;
    }
    r.frame = frame;
    const int w = static_cast<int>(image.getWidth()), h = static_cast<int>(image.getHeight());
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
        if (keep && mono) { im->center = center; im->width = width; }
    }
    else
    {
        const Uint8* out = static_cast<const Uint8*>(image.getOutputData(8, 0, 0));
        if (!out || w <= 0 || h <= 0)
        {
            im->image.reset();
            decoded.reset();
            return L"its pixels could not be rendered";
        }
        const size_t n = static_cast<size_t>(w) * h;
        im->color.resize(n);
        for (size_t i = 0; i < n; i++)
            im->color[i] = (static_cast<DWORD>(out[3 * i]) << 16) | (out[3 * i + 1] << 8) | out[3 * i + 2];
        im->center0 = 127.5;
        im->width0 = 255;
        if (!(keep && !mono)) { im->center = im->center0; im->width = im->width0; }
    }
    r.width = w;
    r.height = h;
    Rewindow(r);
    return std::wstring();
}

// The preview reads the whole file once (Everything: the header too); stepping through a series, each
// file without its header (Image: it is written when the stepping stops). The thumbnail: only pixels.
enum class Render { Thumbnail, Image, Everything };

static Rendered RenderDicom(const std::vector<char>& data, Render what = Render::Everything)
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

    OFString sopClass, tsUid;
    ds->findAndGetOFString(DCM_SOPClassUID, sopClass);
    im->xfer = ds->getOriginalXfer();
    tsUid = DcmXfer(im->xfer).getXferID();
    ff.getMetaInfo()->findAndGetOFString(DCM_TransferSyntaxUID, tsUid);      // (the one of the file: a private one too)
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;
    r.frames = frames;
    // The cine: FrameTime (ms), or the frame rate of the device
    Float64 rate = 0;
    if (ds->findAndGetFloat64(DCM_FrameTime, r.frameTime).bad() || !(r.frameTime > 0)) r.frameTime = 0;
    if (r.frameTime == 0 && ds->findAndGetFloat64(DCM_RecommendedDisplayFrameRate, rate).good() && rate > 0) r.frameTime = 1000 / rate;
    if (r.frameTime == 0 && ds->findAndGetFloat64(DCM_CineRate, rate).good() && rate > 0) r.frameTime = 1000 / rate;

    OFString studyUid, seriesNumber;
    ds->findAndGetOFString(DCM_StudyInstanceUID, studyUid);
    ds->findAndGetOFString(DCM_SeriesNumber, seriesNumber);
    r.studyUid = studyUid.c_str();
    r.seriesNumber = seriesNumber.c_str();

    if (what == Render::Everything) DumpHeader(ff, r.tags, r.names);

    r.summary = sopClass.empty() ? L"DICOM" : ObjectType(sopClass.c_str());
    if (what != Render::Thumbnail && ds->tagExists(DCM_DirectoryRecordSequence))
    {
        r.directory = ReadDirectory(ds);
        size_t files = 0;
        std::function<void(const std::vector<DirNode>&)> count = [&](const std::vector<DirNode>& nodes) {
            for (auto& n : nodes) { if (!n.file.empty()) files++; count(n.children); }
        };
        count(r.directory);
        r.summary = L"DICOMDIR  -  " + std::to_wstring(r.directory.size()) + (r.directory.size() == 1 ? L" patient" : L" patients") +
                    L", " + std::to_wstring(files) + L" files";
    }
    r.imaging = im;
    if (ds->tagExists(DCM_PixelData) || IsPmsct(ff) || HasFloatPixels(ds))
    {
        // Only the frame shown is decoded: a multi-frame file can be large
        const std::wstring why = ShowFrame(r, 0, false);
        if (why.empty())
        {
            r.summary += L"  -  " + std::to_wstring(r.width) + L" x " + std::to_wstring(r.height);
            if (frames > 1) r.summary += L"  -  " + std::to_wstring(frames) + L" frames";
        }
        else r.summary += L"  -  image not shown: " + why;
    }
    else r.summary += L"  -  no image";
    r.summary += L"  -  compression: " + Compression(tsUid.c_str());
    if (what == Render::Thumbnail) r.imaging.reset();       // only its pixels
    else Annotate(r, ds);
    return r;
}

// The whole file in memory, up to 1 GB: the other files of the series
static bool ReadPath(const std::wstring& path, std::vector<char>& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= (1LL << 30);
    if (ok)
    {
        data.resize(static_cast<size_t>(size.QuadPart));
        size_t got = 0;
        while (ok && got < data.size())
        {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>((std::min)(data.size() - got, static_cast<size_t>(1) << 24));
            ok = ReadFile(h, data.data() + got, want, &n, nullptr) && n > 0;
            got += n;
        }
        data.resize(got);
    }
    CloseHandle(h);
    return ok;
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

// A preview asks for its series in a thread of its own (a folder can have thousands of files), which
// posts `message` to its window when it has it
struct SeriesScan
{
    std::wstring path, folder;
    HWND window = nullptr;
    UINT message = 0;
    volatile LONG cancel = 0;
    std::vector<std::wstring> files;                // written before the message, read after it
};

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
        if (previous) return previous->SetWindow(hwnd, rc);
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
        if (previous) return previous->SetRect(rc);
        if (wnd) Place();
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() override
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
    IFACEMETHODIMP Unload() override
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
        playing = stale = windowSet = plain = false;
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
        if (previous) return previous->SetFocus();
        HWND content = Content();
        if (!content) return S_FALSE;
        ::SetFocus(content);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* focus) override
    {
        if (!focus) return E_INVALIDARG;
        if (previous) return previous->QueryFocus(focus);
        *focus = GetFocus();
        return *focus ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override
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
    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override
    {
        backColor = color;
        if (background) { DeleteObject(background); background = nullptr; }
        Recolor();
        if (IPreviewHandlerVisuals* v = PreviousVisuals()) { v->SetBackgroundColor(color); v->Release(); }
        return S_OK;
    }
    IFACEMETHODIMP SetFont(const LOGFONTW* f) override
    {
        if (f) { hostFont = *f; hasFont = true; }
        if (IPreviewHandlerVisuals* v = PreviousVisuals()) { v->SetFont(f); v->Release(); }
        return S_OK;
    }
    IFACEMETHODIMP SetTextColor(COLORREF color) override
    {
        textColor = color;
        Recolor();
        if (IPreviewHandlerVisuals* v = PreviousVisuals()) { v->SetTextColor(color); v->Release(); }
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
        IObjectWithSite* p = nullptr;
        if (previous && SUCCEEDED(previous->QueryInterface(IID_PPV_ARGS(&p)))) { p->SetSite(s); p->Release(); }
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return site ? site->QueryInterface(riid, ppv) : E_FAIL;
    }

    // The DICOM explorer hosts it in its window: only the image (no tabs), of the files it gives, and
    // tells it which one is shown when the slider or the cine moves (`moved`)
    HRESULT Host(HWND host, const RECT& rc)
    {
        hosted = true;
        parent = host;
        bounds = rc;
        return CreateWindows();
    }
    // (frames: of each file, which one of an enhanced multi-frame file it is; -1: the file as it is)
    void ShowFiles(const std::vector<std::wstring>& files, int index, const std::vector<int>& frames = {})
    {
        StopPoster();
        Play(false);
        windowSet = false;
        zoom = 1;
        series = files;
        seriesFrames = frames;
        if (std::none_of(seriesFrames.begin(), seriesFrames.end(), [](int f) { return f >= 0; })) seriesFrames.clear();
        slice = -1;
        group = -1;
        if (index >= 0 && index < static_cast<int>(files.size()))
        {
            GoSlice(index);
            return;
        }
        const bool had = !view.pixels.empty();
        view = Rendered();
        view.summary = L"No image";
        current.clear();
        Shown(had);
    }
    void ShowIndex(int index) { GoSlice(index); }    // another one of the same files
    std::function<void(int)> moved;
    // The explorer's thumbnail of a node (0xRRGGBB pixels, its frames moving), in place of the first of
    // its files until the slider, the keys or the cine go to them
    struct PosterFrame { int width = 0, height = 0; std::vector<DWORD> px; };
    void ShowPoster(std::vector<PosterFrame> frames, int delay, const std::wstring& caption)
    {
        if (!hosted || frames.empty()) return;
        Play(false);
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
    bool DraggedWindow(std::wstring& file, double& center, double& width) const
    {
        if (!windowSet || !view.imaging || !view.imaging->image || !view.imaging->mono) return false;
        file = current;
        center = view.imaging->center;
        width = view.imaging->width;
        return true;
    }

private:
    enum { TabColumns, TabHeader, TabTags, TabDirectory, TabCount };
    enum class Drag { None, Split, Window, Pan, Zoom, Track };
    static constexpr UINT WM_READCOLUMNS = WM_APP + 1, WM_SERIES = WM_APP + 2;
    static constexpr UINT_PTR TimerCine = 1, TimerSettle = 2, TimerPoster = 3;
    std::vector<PosterFrame> poster;                // hosted: the thumbnail shown before the files (none: empty)
    std::vector<int> seriesFrames;                  // hosted: the frame of each file of `series` (empty: none)
    size_t posterFrame = 0;

    void PosterPixels()
    {
        const PosterFrame& p = poster[posterFrame % poster.size()];
        view.width = p.width;
        view.height = p.height;
        view.pixels = p.px;
    }

    void StopPoster()
    {
        if (poster.empty()) return;
        KillTimer(wnd, TimerPoster);
        poster.clear();
    }
    static constexpr const wchar_t* Settings = L"Software\\DicomStudyCount\\Preview";

    LONG refs = 1;
    IShellItem* item = nullptr;
    IUnknown* site = nullptr;
    IPreviewHandler* previous = nullptr;            // the preview from before, for a file that is not DICOM
    LOGFONTW hostFont = {};
    bool hasFont = false;

    IPreviewHandlerVisuals* PreviousVisuals() const
    {
        IPreviewHandlerVisuals* v = nullptr;
        if (previous) previous->QueryInterface(IID_PPV_ARGS(&v));
        return v;
    }

    // The preview from before for this extension, in this window: initialized as it can be (the item,
    // the path or the stream), with the site and colors of this one
    bool StartPrevious(IStream* stream)
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
    std::wstring path, folder, note;
    std::wstring current;                           // the file shown: the path, or another one of its series
    std::shared_ptr<SeriesScan> scan;
    std::vector<std::wstring> series;               // the files of its series, in order (empty until read)
    int slice = -1;                                 // the one shown, in `series`
    bool overlay = true, playing = false;
    bool plain = false;                             // not DICOM, and nothing before: only a line saying so
    bool hosted = false;                            // in the DICOM explorer: the image alone
    bool stale = false;                             // its header and values are those of a slice shown before
    bool windowSet = false;                         // the window dragged: it stays from slice to slice
    HWND parent = nullptr, wnd = nullptr, edit = nullptr, list = nullptr, tree = nullptr;
    // A DICOMDIR: its records, and the files under each record that has them (a series, mostly): the
    // slider steps through those of the one chosen
    struct DirGroup { std::vector<std::wstring> files, labels; std::vector<HTREEITEM> items; };
    std::vector<DirNode> directory;
    std::vector<DirGroup> groups;
    int group = -1;
    bool selecting = false;                         // the tree selection changes from the code
    RECT bounds = {};
    RECT tabArea = {}, tabRects[TabCount] = {}, contentArea = {}, splitArea = {}, imageArea = {}, statusArea = {};
    RECT barArea = {}, playRect = {}, trackRect = {}, overlayRect = {};
    int tab = TabColumns;
    int split = 450;                                // the share of the top panel, in thousandths
    Drag drag = Drag::None;
    POINT dragFrom = {};
    double dragCenter = 0, dragWidth = 0, dragViewX = 0, dragViewY = 0, dragZoom = 1;
    double zoom = 1, viewX = 0, viewY = 0;          // the zoom over the fit, and the image point in the middle
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

    HWND Content() const { return tab == TabColumns ? list : tab == TabDirectory ? tree : edit; }

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
        if (!folder.empty() && !hosted)
            tab = static_cast<int>(ReadDword(L"Software\\DicomStudyCount\\Preview\\Tabs", folder.c_str(), TabColumns));
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
    void FillTree()
    {
        SendMessageW(tree, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), FALSE);
        TreeView_SetExtendedStyle(tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        for (const DirNode& n : directory) AddNode(TVI_ROOT, n);
        if (HTREEITEM first = TreeView_GetRoot(tree)) TreeView_EnsureVisible(tree, first);
    }

    static LPARAM Code(int g, int i) { return static_cast<LPARAM>((static_cast<long long>(g) << 32) | static_cast<unsigned>(i)); }
    static int GroupOf(LPARAM p) { return static_cast<int>(static_cast<long long>(p) >> 32); }
    static int FileOf(LPARAM p) { return static_cast<int>(static_cast<unsigned>(p & 0xFFFFFFFF)); }

    // A record: those whose records are all files become a group (their tree items come when it is
    // opened: a series can have thousands); a file alone is a group of its own
    void AddNode(HTREEITEM under, const DirNode& n)
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
    void ExpandGroup(int g, HTREEITEM node)
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
    void Choose(LPARAM p)
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
    void Reveal(LPARAM p)
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
    void FillValues(const std::wstring& file)
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
        if (edit) ShowWindow(edit, tab == TabHeader || tab == TabTags ? SW_SHOWNA : SW_HIDE);
        if (tree) ShowWindow(tree, tab == TabDirectory ? SW_SHOWNA : SW_HIDE);
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
        if (tree)
        {
            TreeView_SetBkColor(tree, backColor);
            TreeView_SetTextColor(tree, textColor);
            SetWindowTheme(tree, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            InvalidateRect(tree, nullptr, TRUE);
        }
        if (wnd) InvalidateRect(wnd, nullptr, TRUE);
    }

    void Place()
    {
        SetWindowPos(wnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
    }

    // From the top: the tabs, their content, the bar that moves the split, the image, the image bar
    // (frames or slices, the overlay), the status line (the summary, and the window of the image).
    // Without an image the content takes their place.
    void Layout()
    {
        RECT c;
        GetClientRect(wnd, &c);
        const int w = c.right, h = c.bottom, line = Px(22), bar = Px(6);
        tabArea = { 0, 0, w, hosted ? 0 : line + Px(4) };
        statusArea = { Px(4), h - line, w - Px(4), h };
        const int top = tabArea.bottom, bottom = h - line > top ? h - line : top;
        if (hosted)
        {
            // The image over all of it, its bar under it
            const int image = bottom - line > top ? bottom - line : top;
            contentArea = splitArea = {};
            imageArea = { 0, top, w, image };
            barArea = { 0, image, w, bottom };
        }
        else if (!view.pixels.empty())
        {
            const int image = bottom - line > top ? bottom - line : top;
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

    void Paint(HDC dc)
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
    void PaintOverlay(HDC dc, const RECT& r)
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

    // The image bar: play / pause and the slider of the frames (of a multi-frame file) or of the slices
    // (of its series) with where it is, and the check box of the overlay at the right
    void PaintBar(HDC dc)
    {
        playRect = trackRect = overlayRect = {};
        if (barArea.bottom <= barArea.top) return;
        SelectObject(dc, uiFont ? uiFont : GetStockObject(DEFAULT_GUI_FONT));
        const int cy = (barArea.top + barArea.bottom) / 2;
        int left = barArea.left + Px(6), right = barArea.right - Px(6);
        HBRUSH accent = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT)), ink = CreateSolidBrush(textColor);
        HBRUSH groove = CreateSolidBrush(Dim(30)), border = CreateSolidBrush(Dim(60));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN)), oldBrush = SelectObject(dc, ink);

        static const wchar_t label[] = L"Overlay";
        SIZE ext = {};
        GetTextExtentPoint32W(dc, label, ARRAYSIZE(label) - 1, &ext);
        const int box = Px(13);
        overlayRect = { right - ext.cx - Px(6) - box, barArea.top, right, barArea.bottom };
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
        RECT text = { square.right + Px(6), barArea.top, right, barArea.bottom };
        DrawTextW(dc, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        right = overlayRect.left - Px(14);

        const int n = Count(), at = poster.empty() ? Position() : 0;     // (the thumbnail: the slider at the first file)
        if (n > 1 && at >= 0)
        {
            const int s = Px(10);
            playRect = { left, barArea.top, left + Px(20), barArea.bottom };
            const int px = (playRect.left + playRect.right) / 2;
            if (playing)
            {
                RECT a = { px - s / 2, cy - s / 2, px - s / 2 + s * 35 / 100, cy - s / 2 + s };
                RECT b = { px + s / 2 - s * 35 / 100, cy - s / 2, px + s / 2, cy - s / 2 + s };
                FillRect(dc, &a, ink);
                FillRect(dc, &b, ink);
            }
            else
            {
                POINT play[3] = { { px - s / 2 + 1, cy - s / 2 }, { px - s / 2 + 1, cy + s / 2 }, { px + s / 2, cy } };
                Polygon(dc, play, 3);
            }
            left = playRect.right + Px(8);

            wchar_t where[96];
            if (view.frames > 1 && series.size() > 1 && seriesFrames.empty())
                swprintf_s(where, L"Frame %d / %d   File %d / %d", at + 1, n, slice + 1, static_cast<int>(series.size()));
            else if (!poster.empty()) swprintf_s(where, L"Thumbnail   %d files", n);
            else swprintf_s(where, view.frames > 1 ? L"Frame %d / %d" : L"Slice %d / %d", at + 1, n);
            GetTextExtentPoint32W(dc, where, static_cast<int>(wcslen(where)), &ext);
            RECT w = { right - ext.cx, barArea.top, right, barArea.bottom };
            if (w.left > left + Px(40))
            {
                ::SetTextColor(dc, Dim(70));
                DrawTextW(dc, where, -1, &w, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                right = w.left - Px(12);
            }
            if (right - left > Px(30))
            {
                trackRect = { left, barArea.top, right, barArea.bottom };
                const int x = left + static_cast<int>(static_cast<long long>(right - left) * at / (n - 1));
                RECT track = { left, cy - Px(1), right, cy - Px(1) + Px(2) }, done = { left, track.top, x, track.bottom };
                FillRect(dc, &track, groove);
                FillRect(dc, &done, accent);
                SelectObject(dc, accent);
                const int k = Px(5);
                Ellipse(dc, x - k, cy - k, x + k + 1, cy + k + 1);
            }
        }
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        for (HBRUSH b : { accent, ink, groove, border }) DeleteObject(b);
    }

    // The slider of the image bar: the frames of a multi-frame file, or else the files of its series
    // (the frames of enhanced files, as images of their own: the slider over them, as over files)
    int Count() const { return view.frames > 1 && seriesFrames.empty() ? view.frames : static_cast<int>(series.size()); }
    int Position() const { return view.frames > 1 && seriesFrames.empty() ? view.frame : slice; }
    void GoTo(int i)
    {
        if (view.frames > 1 && seriesFrames.empty()) GoFrame(i);
        else GoSlice(i);
    }
    void Step(int delta)
    {
        const int n = Count();
        if (!poster.empty() && n > 0) GoTo(0);
        else if (n > 1 && Position() >= 0) GoTo((std::max)(0, (std::min)(n - 1, Position() + delta)));
    }

    // Another frame of the file, with the same window
    void GoFrame(int f)
    {
        if (f < 0 || f >= view.frames || f == view.frame || !view.imaging) return;
        const bool had = !view.pixels.empty();
        std::wstring why;
        try { why = ShowFrame(view, f, true); }
        catch (...) { why = L"it cannot be shown"; }
        if (!why.empty()) view.frame = f;           // no image, and the stepping goes on from there
        Shown(had);
    }

    // Another file of the series: the window dragged stays, and the zoom while the size of the image
    // does. Its header and the values of its columns, when the stepping stops (Settle).
    void GoSlice(int i)
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
    void Shown(bool had)
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
    void Settle()
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
    void Series()
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

    // The cine: the frames at the pace of the file, the slices at 10 a second, round and round
    void Play(bool on)
    {
        if (on && !poster.empty()) GoSlice(0);     // (from the thumbnail: the files, from the first one)
        playing = on && Count() > 1;
        if (playing)
        {
            const double ms = view.frames > 1 && view.frameTime > 0 ? view.frameTime : 100;
            SetTimer(wnd, TimerCine, static_cast<UINT>((std::max)(15.0, (std::min)(1000.0, ms))), nullptr);
        }
        else
        {
            KillTimer(wnd, TimerCine);
            if (stale) SetTimer(wnd, TimerSettle, 300, nullptr);     // the header of the slice it stopped at
        }
        InvalidateRect(wnd, &barArea, FALSE);
    }

    // The keys of the image, when it has the focus (a click on it): Left / Right the frames (or the
    // slices), Up / Down and Page Up / Page Down the slices, Home / End the first and the last, Space
    // the cine
    bool Key(WPARAM key)
    {
        if (view.pixels.empty() && Count() < 2) return false;
        switch (key)
        {
        case VK_LEFT: Step(-1); return true;
        case VK_RIGHT: Step(1); return true;
        case VK_UP: case VK_PRIOR: GoSlice(slice - 1); return true;
        case VK_DOWN: case VK_NEXT: GoSlice(slice + 1); return true;
        case VK_HOME: GoTo(0); return true;
        case VK_END: GoTo(Count() - 1); return true;
        case VK_SPACE: Play(!playing); return true;
        }
        return false;
    }

    // Dragging along the slider
    void TrackTo(POINT pt)
    {
        const int n = Count();
        if (n < 2 || trackRect.right <= trackRect.left) return;
        const double t = static_cast<double>(pt.x - trackRect.left) / (trackRect.right - trackRect.left);
        GoTo(static_cast<int>(std::lround((std::max)(0.0, (std::min)(1.0, t)) * (n - 1))));
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
        windowSet = true;
        Redraw();
    }

    // Screen pixels per image pixel: the fit into the image area, times the zoom
    double Scale() const
    {
        if (view.pixels.empty() || view.width <= 0 || view.height <= 0) return 0;
        const double fit = (std::min)(static_cast<double>(imageArea.right - imageArea.left) / view.width,
                                      static_cast<double>(imageArea.bottom - imageArea.top) / view.height);
        return fit > 0 ? fit * zoom : 0;
    }

    // The point of the image at the center of the area: inside the image, the middle of it unzoomed
    void KeepView()
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
    void ZoomTo(POINT pt)
    {
        SetZoom(dragFrom, dragZoom * std::pow(2.0, (dragFrom.y - pt.y) / static_cast<double>(Px(100))));
    }

    // A new zoom (1 to 64), the image point at `pt` staying there
    void SetZoom(POINT pt, double next)
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
    void StartImageDrag(Drag what, POINT pt)
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
    void PanTo(POINT pt)
    {
        const double scale = Scale();
        if (scale <= 0) return;
        viewX = dragViewX - (pt.x - dragFrom.x) / scale;
        viewY = dragViewY - (pt.y - dragFrom.y) / scale;
        KeepView();
        InvalidateRect(wnd, &imageArea, FALSE);
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
        case WM_SERIES:
            if (self && self->scan) self->Series();
            return 0;
        case WM_TIMER:
            if (self && wp == TimerSettle)
            {
                if (self->playing) KillTimer(hwnd, TimerSettle);     // again when it stops
                else self->Settle();
            }
            else if (self && wp == TimerCine)
            {
                const int n = self->Count();
                if (n < 2 || self->Position() < 0) self->Play(false);
                else self->GoTo((self->Position() + 1) % n);
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
                for (const RECT* r : { &self->playRect, &self->trackRect, &self->overlayRect })
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
                else if (PtInRect(&self->overlayRect, pt))
                {
                    self->overlay = !self->overlay;
                    DWORD value = self->overlay ? 1 : 0;
                    RegSetKeyValueW(HKEY_CURRENT_USER, Settings, L"Overlay", REG_DWORD, &value, sizeof(value));
                    InvalidateRect(hwnd, &self->imageArea, FALSE);
                    InvalidateRect(hwnd, &self->barArea, FALSE);
                }
                else if (PtInRect(&self->trackRect, pt))
                {
                    self->drag = Drag::Track;
                    SetCapture(hwnd);
                    self->TrackTo(pt);
                }
                else if (self->view.imaging && PtInRect(&self->imageArea, pt))
                    self->StartImageDrag((wp & MK_SHIFT) ? Drag::Pan : Drag::Window, pt);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (self && self->drag != Drag::None)
            {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                if (self->drag == Drag::Window) self->MoveWindowTo(pt);
                else if (self->drag == Drag::Pan) self->PanTo(pt);
                else if (self->drag == Drag::Zoom) self->ZoomTo(pt);
                else if (self->drag == Drag::Track) self->TrackTo(pt);
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
            // Not DICOM: the thumbnail of the provider from before (none: the icon)
            if (!StreamLooksDicom(stream)) return PreviousThumbnail(cx, bitmap, alpha);
            std::vector<char> data;
            HRESULT hr = ReadAll(stream, data);
            if (FAILED(hr)) return hr;
            Rendered r = RenderDicom(data, Render::Thumbnail);
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

    // The provider from before for the extension of the file of the stream, given the same stream
    HRESULT PreviousThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
    {
        const std::wstring name = StreamName(stream);
        if (name.empty()) return E_FAIL;
        IThumbnailProvider* p = CreatePrevious<IThumbnailProvider>(Previous(HKEY_CURRENT_USER, ChainKey, name, L"Thumbnail"));
        if (!p) return E_FAIL;
        HRESULT hr = E_NOINTERFACE;
        IInitializeWithStream* init = nullptr;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&init))))
        {
            hr = init->Initialize(stream, STGM_READ);
            init->Release();
        }
        if (SUCCEEDED(hr)) hr = p->GetThumbnail(cx, bitmap, alpha);
        p->Release();
        return hr;
    }
};

// ---------------------------------------------------------------------------------------------
// DICOM explorer (rundll32 DicomShell.dll,Explore <folder>; "DICOM explorer" in the DICOM menu of a
// folder): the DICOM files under a folder as the tree of DCMexplorer.m / DICOMgather.m (mTools) -
// patient, study, series, orientation (the plane and size of the images), position (the place of the
// slice) and image. Beside the tree: what the chosen node is (its counts and main values), its image
// (the preview, its slider over the files under the node, with the cine) and its full info: the
// attributes all its files share, level by level (those of the image, those every image of its position
// shares, those of its orientation, of its series...), and those that vary in it.

// The values of the attributes of all the files, each text once (the files of a series share most)
class ValuePool
{
public:
    Uint32 Id(const std::wstring& v)
    {
        AcquireSRWLockExclusive(&lock);
        Uint32 id = 0;
        auto it = ids.find(v);
        if (it != ids.end()) id = it->second;
        else
        {
            id = static_cast<Uint32>(texts.size());
            texts.push_back(v);
            ids.emplace(v, id);
        }
        ReleaseSRWLockExclusive(&lock);
        return id;
    }
    // (once the gathering is over: nothing writes then)
    const std::wstring& Text(Uint32 id) const { return texts[id]; }

private:
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<std::wstring, Uint32> ids;
    std::vector<std::wstring> texts;
};

using Attributes = std::vector<std::pair<Uint32, Uint32>>;      // (tag, its value in the pool), by tag

// A DICOM file of the explorer: what places it in the tree, and all its attributes
struct Gathered
{
    std::wstring path;
    std::string charset, patient, patientName, birthDate, study, studyDate, studyTime, studyDescription;
    std::string series, seriesNumber, seriesDate, seriesTime, seriesDescription, modality;
    std::string instance, trigger, time, orientationText, positionText;
    double orientation[6] = {}, position[3] = {}, z = 0;
    bool oriented = false, placed = false, hasZ = false;
    unsigned rows = 0, columns = 0;
    long frames = 1;                                // (a frame of an enhanced file: all of them in its first one, 0 in the rest)
    int frame = -1;                                 // a frame of an enhanced multi-frame file (-1: the whole file)
    long long bytes = 0;                            // (a frame: the size of the file in its first one, 0 in the rest)
    Attributes attributes;
};

// The value of a tag among the attributes (sorted by tag): put, or changed
static void SetAttribute(Attributes& a, Uint32 tag, Uint32 id)
{
    auto it = std::lower_bound(a.begin(), a.end(), tag, [](const std::pair<Uint32, Uint32>& x, Uint32 t) { return x.first < t; });
    if (it != a.end() && it->first == tag) it->second = id;
    else a.insert(it, { tag, id });
}

// Who the patient of a file is, beyond its PatientID: its birth date (its digits) and the words of its
// name, in capitals, without accents and in alphabetical order ("Garcia^Juan", "JUAN GARCIA" and "garcia,
// juan" are the same name, with an accent on the i too). Some exports put the same placeholder in the PatientID of everyone (9999), and files
// without one all have the same empty one: these tell their people apart.
struct Person
{
    std::string birth;
    std::wstring name;                              // its words, in order, one space between them
    std::vector<std::wstring> words;
    bool Empty() const { return birth.empty() && name.empty(); }
};

static Person PersonOf(const std::string& name, const std::string& charset, const std::string& birth)
{
    Person p;
    for (char c : birth) if (c >= '0' && c <= '9') p.birth += c;
    const std::wstring w = Widen(name, charset);
    // Decomposed, the accents are marks of their own (not letters), and go
    std::wstring d = w;
    const int n = NormalizeString(NormalizationD, w.c_str(), static_cast<int>(w.size()), nullptr, 0);
    if (n > 0)
    {
        d.assign(static_cast<size_t>(n), L'\0');
        const int k = NormalizeString(NormalizationD, w.c_str(), static_cast<int>(w.size()), &d[0], n);
        d.resize(k > 0 ? static_cast<size_t>(k) : 0);
    }
    // Its words (what is not a letter separates them), in alphabetical order: "JUAN GARCIA" is "GARCIA^JUAN"
    std::wstring word;
    for (wchar_t c : d + L" ")
    {
        if (IsCharAlphaW(c)) { word += c; continue; }
        if (word.empty()) continue;
        CharUpperBuffW(&word[0], static_cast<DWORD>(word.size()));
        p.words.push_back(word);
        word.clear();
    }
    std::sort(p.words.begin(), p.words.end());
    p.words.erase(std::unique(p.words.begin(), p.words.end()), p.words.end());
    for (const auto& w2 : p.words) p.name += (p.name.empty() ? L"" : L" ") + w2;
    return p;
}

// The same person, as far as they tell: what both have agrees. A missing birth date or name does not
// tell them apart, nor a name with fewer words than the other one (a second name, or surname, left out),
// nor an initial for a word ("R" for "RAVI"). With the same birth date in both, a word in common is enough
// (a name written two ways); and with only the year of the birth date apart, a word in common too (a
// mistake in the year, or an anonymizer that moves it)
static bool SamePerson(const Person& a, const Person& b)
{
    auto common = [&]() {
        for (const auto& w : a.words) if (std::binary_search(b.words.begin(), b.words.end(), w)) return true;
        return false;
    };
    if (!a.birth.empty() && !b.birth.empty() && a.birth != b.birth)
        return a.birth.size() == 8 && b.birth.size() == 8 && a.birth.compare(4, 4, b.birth, 4, 4) == 0 && common();
    if (a.words.empty() || b.words.empty()) return true;
    if (!a.birth.empty() && a.birth == b.birth)
        for (const auto& w : a.words)
            if (std::binary_search(b.words.begin(), b.words.end(), w)) return true;
    const auto& few = a.words.size() <= b.words.size() ? a.words : b.words;
    const auto& many = a.words.size() <= b.words.size() ? b.words : a.words;
    std::vector<bool> used(many.size(), false);
    for (const auto& w : few)
    {
        size_t k = 0;
        for (; k < many.size(); k++)
        {
            if (used[k]) continue;
            const auto& x = many[k];
            if (x == w || (w.size() == 1 && x[0] == w[0]) || (x.size() == 1 && w[0] == x[0])) break;
        }
        if (k == many.size()) return false;
        used[k] = true;
    }
    return true;
}

enum class Found { Other, Dicom, Directory };

// A file, if it is DICOM (a DICOMDIR is not one of the files): its header up to the pixels, by DCMTK.
// Values of more than 4 KB are not read: their size stands for them.
static Found GatherFile(const std::wstring& path, long long bytes, Gathered& g, std::vector<Gathered>& more, ValuePool& pool)
{
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Found::Other;
        unsigned char head[132];
        DWORD n = 0;
        const BOOL ok = ReadFile(h, head, sizeof(head), &n, nullptr);
        CloseHandle(h);
        if (!ok || !LooksDicom(head, n)) return Found::Other;
    }
    DcmFileFormat ff;
    if (ff.loadFileUntilTag(OFFilename(path.c_str(), OFTrue), EXS_Unknown, EGL_noChange, 4096, ERM_autoDetect, DCM_PixelData).bad())
        return Found::Other;
    DcmDataset* ds = ff.getDataset();
    auto get = [&](const DcmTagKey& key) {
        OFString v;
        ds->findAndGetOFStringArray(key, v);
        return std::string(v.c_str(), v.length());
    };
    if (get(DCM_SOPClassUID) == DirectorySopClass || ds->tagExists(DCM_DirectoryRecordSequence)) return Found::Directory;
    g.path = path;
    g.bytes = bytes;
    g.charset = get(DCM_SpecificCharacterSet);
    g.patient = get(DCM_PatientID);
    g.patientName = get(DCM_PatientName);           // (only to tell apart people that share a PatientID)
    g.birthDate = get(DCM_PatientBirthDate);
    g.study = get(DCM_StudyInstanceUID);
    g.studyDate = get(DCM_StudyDate);
    g.studyTime = get(DCM_StudyTime);
    g.studyDescription = get(DCM_StudyDescription);
    g.series = get(DCM_SeriesInstanceUID);
    g.seriesNumber = get(DCM_SeriesNumber);
    g.seriesDate = get(DCM_SeriesDate);
    g.seriesTime = get(DCM_SeriesTime);
    g.seriesDescription = get(DCM_SeriesDescription);
    g.modality = get(DCM_Modality);
    g.instance = get(DCM_InstanceNumber);
    g.trigger = get(DCM_TriggerTime);
    g.time = get(DCM_AcquisitionDateTime);
    if (g.time.empty()) g.time = get(DCM_AcquisitionDate) + get(DCM_AcquisitionTime);
    if (g.time.empty()) g.time = get(DCM_ContentDate) + get(DCM_ContentTime);
    g.orientationText = get(DCM_ImageOrientationPatient);
    g.positionText = get(DCM_ImagePositionPatient);
    g.oriented = Numbers(g.orientationText, g.orientation, 6);
    g.placed = Numbers(g.positionText, g.position, 3);
    Header h;
    h.imageOrientation = g.orientationText;
    h.imagePosition = g.positionText;
    g.hasZ = ZLevel(h, g.z);
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    g.rows = rows;
    g.columns = columns;
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).good() && frames > 1) g.frames = frames;
    // All the attributes: the meta information, then the dataset (in the order of their tags)
    for (DcmItem* item : { static_cast<DcmItem*>(ff.getMetaInfo()), static_cast<DcmItem*>(ds) })
        for (unsigned long i = 0; item && i < item->card(); i++)
        {
            DcmElement* e = item->getElement(i);
            if (!e) continue;
            const DcmTag& tag = e->getTag();
            std::wstring v;
            if (e->ident() == EVR_SQ)
            {
                const unsigned long k = static_cast<DcmSequenceOfItems*>(e)->card();
                v = L"(" + std::to_wstring(k) + (k == 1 ? L" item)" : L" items)");
            }
            else
            {
                v = ValueOf(e, item == ds ? g.charset : std::string());
                if (v.size() >= 2 && v.front() == L'[' && v.back() == L']') v = v.substr(1, v.size() - 2);
            }
            g.attributes.emplace_back((static_cast<Uint32>(tag.getGTag()) << 16) | tag.getETag(), pool.Id(v));
        }
    std::sort(g.attributes.begin(), g.attributes.end());

    // Enhanced multi-frame (a volume, a cine, in one file): each frame an image of its own, with the
    // position, orientation and trigger time of its functional groups (its own, or the shared ones)
    DcmSequenceOfItems* perFrame = nullptr;
    if (g.frames > 1 && ds->findAndGetSequence(DCM_PerFrameFunctionalGroupsSequence, perFrame).good() && perFrame &&
        static_cast<long>(perFrame->card()) == g.frames)
    {
        DcmSequenceOfItems* sharedSeq = nullptr;
        DcmItem* shared = ds->findAndGetSequence(DCM_SharedFunctionalGroupsSequence, sharedSeq).good() && sharedSeq && sharedSeq->card()
                              ? sharedSeq->getItem(0) : nullptr;
        auto inGroup = [](DcmItem* group, const DcmTagKey& sequence, const DcmTagKey& key) {
            DcmSequenceOfItems* s = nullptr;
            OFString v;
            if (group && group->findAndGetSequence(sequence, s).good() && s && s->card()) s->getItem(0)->findAndGetOFStringArray(key, v);
            return std::string(v.c_str(), v.length());
        };
        auto value = [&](DcmItem* item, const DcmTagKey& sequence, const DcmTagKey& key) {
            const std::string v = inGroup(item, sequence, key);
            return v.empty() ? inGroup(shared, sequence, key) : v;
        };
        const Gathered base = g;
        const long total = g.frames;
        more.reserve(static_cast<size_t>(total - 1));
        for (long k = 0; k < total; k++)
        {
            if (k > 0) more.push_back(base);
            Gathered& e = k == 0 ? g : more.back();
            DcmItem* item = perFrame->getItem(static_cast<unsigned long>(k));
            e.frame = static_cast<int>(k);
            e.frames = k == 0 ? total : 0;
            e.bytes = k == 0 ? bytes : 0;
            const std::string ipp = value(item, DCM_PlanePositionSequence, DCM_ImagePositionPatient);
            const std::string iop = value(item, DCM_PlaneOrientationSequence, DCM_ImageOrientationPatient);
            const std::string delay = value(item, DCM_CardiacSynchronizationSequence, DCM_NominalCardiacTriggerDelayTime);
            if (!ipp.empty()) e.positionText = ipp;
            if (!iop.empty()) e.orientationText = iop;
            if (!delay.empty()) e.trigger = delay;
            e.oriented = Numbers(e.orientationText, e.orientation, 6);
            e.placed = Numbers(e.positionText, e.position, 3);
            Header fh;
            fh.imageOrientation = e.orientationText;
            fh.imagePosition = e.positionText;
            e.hasZ = ZLevel(fh, e.z);
            // (and among its attributes, where the full info shows them varying from frame to frame)
            if (!ipp.empty()) SetAttribute(e.attributes, 0x00200032, pool.Id(Widen(ipp)));
            if (!iop.empty()) SetAttribute(e.attributes, 0x00200037, pool.Id(Widen(iop)));
            if (!delay.empty()) SetAttribute(e.attributes, 0x00181060, pool.Id(Widen(delay)));
            const std::string spacing = value(item, DCM_PixelMeasuresSequence, DCM_PixelSpacing);
            if (!spacing.empty()) SetAttribute(e.attributes, 0x00280030, pool.Id(Widen(spacing)));
        }
    }
    return Found::Dicom;
}

// The files under a folder, found and read in a thread of its own (and the reading in a few more)
struct Gathering
{
    std::wstring root;
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> cancel{ false }, listing{ true };
    std::atomic<long> found{ 0 }, done{ 0 };
    std::vector<Gathered> files;                    // the DICOM ones (written before the message)
    ValuePool pool;
    long others = 0, directories = 0, offline = 0;
    double seconds = 0;
    std::thread thread;
};

static void Gather(Gathering* g)
{
    const ULONGLONG start = GetTickCount64();
    struct Item { std::wstring path; long long bytes; };
    std::vector<Item> items;
    // The folders depth first, in the order of their names (as Explorer shows them)
    std::vector<std::wstring> folders{ g->root };
    while (!folders.empty() && !g->cancel)
    {
        const std::wstring folder = std::move(folders.back());
        folders.pop_back();
        const std::wstring base = folder + (!folder.empty() && folder.back() == L'\\' ? L"" : L"\\");
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> inside;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // (not into links: they can loop)
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..") && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    inside.push_back(base + fd.cFileName);
                continue;
            }
            // Not downloaded (cloud files): reading them would download them
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) { g->offline++; continue; }
            items.push_back({ base + fd.cFileName, (static_cast<long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
            g->found++;
        } while (!g->cancel && FindNextFileW(find, &fd));
        FindClose(find);
        std::sort(inside.begin(), inside.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) > 0; });
        folders.insert(folders.end(), inside.begin(), inside.end());
    }
    g->listing = false;

    std::vector<Gathered> read(items.size());
    std::vector<std::vector<Gathered>> frames(items.size());     // (the frames of enhanced files after their first one)
    std::vector<Found> kinds(items.size(), Found::Other);
    std::atomic<size_t> next{ 0 };
    auto work = [&]() {
        for (;;)
        {
            const size_t i = next++;
            if (i >= items.size() || g->cancel) break;
            try { kinds[i] = GatherFile(items[i].path, items[i].bytes, read[i], frames[i], g->pool); }
            catch (...) { kinds[i] = Found::Other; }
            g->done++;
        }
    };
    unsigned n = std::thread::hardware_concurrency();
    n = n < 2 ? 2 : n > 6 ? 6 : n;
    std::vector<std::thread> workers;
    for (unsigned k = 0; k < n; k++) workers.emplace_back(work);
    for (auto& w : workers) w.join();
    for (size_t i = 0; i < items.size(); i++)
    {
        if (kinds[i] == Found::Dicom) 
        {
            g->files.push_back(std::move(read[i]));
            for (auto& f : frames[i]) g->files.push_back(std::move(f));
        }
        else if (kinds[i] == Found::Directory) g->directories++;
        else g->others++;
    }
    g->seconds = (GetTickCount64() - start) / 1000.0;
    if (!g->cancel) PostMessageW(g->window, g->message, 0, 0);
}

enum class Level { All, Patient, Study, Series, Orientation, Position, Image };

// A node of the tree: the files under it (in the order of the tree), its children, and those shown in
// the tree (compact: orientations and positions that add nothing are skipped)
struct GatherNode
{
    Level level = Level::All;
    int parent = -1, shownParent = -1;
    std::vector<int> children, shown, files;
    std::wstring text;
    HTREEITEM item = nullptr;
    bool filled = false;                            // its shown children are in the tree
    bool hasCommon = false;
    Attributes common;                              // the attributes all its files share
    std::vector<std::wstring> problems, flags;      // what the checks found in it: in full, and in a word or two
    std::vector<std::wstring> notes;                // what they found that is not a problem by itself
    std::wstring person;                            // a patient whose PatientID other people share: a hash of who it is
    int sharing = 1;                                // a patient: how many people have its PatientID
};

// Values that say who the patient is (and the names of the staff): hidden unless asked for
static bool IdentifyingTag(Uint32 tag)
{
    static const Uint32 tags[] = {
        0x00100010, 0x00100030, 0x00100032, 0x00101000, 0x00101001, 0x00101002, 0x00101005, 0x00101040,
        0x00101060, 0x00102154, 0x00102155, 0x001021B0, 0x00080090, 0x00080092, 0x00080094, 0x00080081,
        0x00081048, 0x00081050, 0x00081060, 0x00081070, 0x00321032, 0x00400006, 0x00380400, 0x00404037 };
    return std::find(std::begin(tags), std::end(tags), tag) != std::end(tags);
}

static std::wstring TagText(Uint32 tag)
{
    wchar_t s[16];
    swprintf_s(s, L"(%04X,%04X)", tag >> 16, tag & 0xFFFF);
    return s;
}

static std::wstring TagName(Uint32 tag)
{
    DcmTag t(static_cast<Uint16>(tag >> 16), static_cast<Uint16>(tag & 0xFFFF));
    std::wstring name = Widen(t.getTagName());
    if (name == Widen(DcmTag_ERROR_TagName)) name.clear();
    if (name.empty() && ((tag >> 16) & 1)) name = L"(private)";
    return name;
}

static std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

static void CopyText(HWND owner, const std::wstring& text)
{
    if (text.empty() || !OpenClipboard(owner)) return;
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

// Explorer windows of the folders of these files, with them selected (the first ten folders)
static void ShowInExplorer(const std::vector<std::wstring>& paths)
{
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> folders;
    for (const auto& p : paths)
    {
        const size_t slash = p.find_last_of(L'\\');
        if (slash == std::wstring::npos) continue;
        const std::wstring folder = p.substr(0, slash == 2 ? 3 : slash);
        auto it = std::find_if(folders.begin(), folders.end(), [&](const auto& f) { return f.first == folder; });
        if (it == folders.end())
        {
            if (folders.size() >= 10) continue;
            folders.push_back({ folder, {} });
            it = folders.end() - 1;
        }
        it->second.push_back(p);
    }
    for (const auto& f : folders)
    {
        PIDLIST_ABSOLUTE dir = ILCreateFromPathW(f.first.c_str());
        if (!dir) continue;
        std::vector<PIDLIST_ABSOLUTE> items;
        std::vector<PCUITEMID_CHILD> children;
        for (const auto& p : f.second)
        {
            if (items.size() >= 4000) break;
            if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(p.c_str()))
            {
                items.push_back(item);
                children.push_back(ILFindLastID(item));
            }
        }
        SHOpenFolderAndSelectItems(dir, static_cast<UINT>(children.size()), children.data(), 0);
        for (auto item : items) ILFree(item);
        ILFree(dir);
    }
}

// ----- Pixels for the explorer: the floating thumbnails of its tree, its exports (PNG, animated GIF,
// NIfTI) and the thumbnails of its report

// One frame of a file: its values (monochrome: the modality values, or the stored ones) or its colors
struct Slice
{
    int width = 0, height = 0;
    bool mono = true, inverse = false;             // inverse: MONOCHROME1, the low values white
    int bits = 0;                                  // monochrome: of the stored values (negative: signed)
    std::vector<float> values;                     // monochrome
    std::vector<DWORD> rgb;                        // color: 0xRRGGBB
    double rowSpacing = 0, columnSpacing = 0;      // PixelSpacing (mm), 0 if not known
    double slope = 1, intercept = 0;               // the rescale of the file
    double center = 0, window = 0;                 // the first window of the header (width 0: none)
    double frameTime = 0;                          // a multi-frame file: ms from frame to frame (0: not known)
    bool isFloat = false;                          // decimal values (Float Pixel Data), in their units
};

// The first number of a value ("40\400": 40)
static bool FirstNumber(const std::string& value, double& out)
{
    char* end = nullptr;
    out = strtod(value.c_str(), &end);
    return end != value.c_str() && std::isfinite(out);
}

// A frame of a file, decoded as the preview decodes it (JPEG 2000 by OpenJPEG). Stored: the values as
// they are in the file, without its rescale. An empty string if it is decoded; if not, why.
static std::wstring DecodeSlice(const std::wstring& path, int frame, bool stored, Slice& s)
{
    s = Slice();
    InitDcmtk();
    DcmFileFormat ff;
    if (ff.loadFile(OFFilename(path.c_str(), OFTrue)).bad()) return L"not readable as DICOM";
    DcmDataset* ds = ff.getDataset();
    const E_TransferSyntax xfer = ds->getOriginalXfer();
    const std::wstring none = NoImage(ff);
    if (!none.empty()) return none;
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;
    if (frame < 0 || frame >= frames) frame = 0;
    auto get = [&](const DcmTagKey& key) {
        OFString v;
        ds->findAndGetOFStringArray(key, v, true);      // (deep: in the functional groups of enhanced files)
        return std::string(v.c_str(), v.length());
    };
    double two[2], v = 0;
    if (Numbers(get(DCM_PixelSpacing), two, 2) && two[0] > 0 && two[1] > 0) { s.rowSpacing = two[0]; s.columnSpacing = two[1]; }
    if (FirstNumber(get(DCM_RescaleSlope), v) && v != 0) s.slope = v;
    if (FirstNumber(get(DCM_RescaleIntercept), v)) s.intercept = v;
    if (FirstNumber(get(DCM_WindowCenter), s.center) && FirstNumber(get(DCM_WindowWidth), v) && v >= 1) s.window = v;
    if (FirstNumber(get(DCM_FrameTime), v) && v > 0) s.frameTime = v;
    else if (FirstNumber(get(DCM_RecommendedDisplayFrameRate), v) && v > 0) s.frameTime = 1000 / v;
    else if (FirstNumber(get(DCM_CineRate), v) && v > 0) s.frameTime = 1000 / v;

    std::unique_ptr<DcmDataset> decoded;
    if (IsJpeg2000(xfer))
    {
        decoded.reset(DecodeJpeg2000(ds, frames > 1, frame));
        if (!decoded) return L"its JPEG 2000 data could not be decoded";
    }
    else if (IsPmsct(ff))
    {
        decoded.reset(DecodePmsct(ds));
        if (!decoded) return L"its PMSCT_RLE1 data could not be decoded";
    }
    else if (IsBitFrames(ds, xfer))
    {
        decoded.reset(DecodeBitFrame(ds, frame));
        if (!decoded) return L"its 1-bit pixels could not be read";
    }
    else if (xfer == EXS_DeflatedImageFrameCompression)
    {
        decoded.reset(DecodeDeflatedFrame(ds, frames > 1, frame));
        if (!decoded) return L"its deflated image frames could not be decoded";
    }
    else if (HasFloatPixels(ds))
    {
        // Decimal numbers: the values as they are, already in their units (no rescale to undo)
        std::vector<double> values;
        Uint16 rows = 0, columns = 0;
        ds->findAndGetUint16(DCM_Rows, rows);
        ds->findAndGetUint16(DCM_Columns, columns);
        if (!FloatFrame(ds, frame, values)) return L"its floating point pixels could not be read";
        s.width = columns;
        s.height = rows;
        s.values.resize(values.size());
        for (size_t i = 0; i < values.size(); i++) s.values[i] = static_cast<float>(values[i]);
        s.isFloat = true;
        s.slope = 1;
        s.intercept = 0;
        s.bits = -32;
        return std::wstring();
    }
    const unsigned long flags = CIF_UsePartialAccessToPixelData | (stored ? CIF_IgnoreModalityTransformation : 0);
    DicomImage image(decoded ? static_cast<DcmObject*>(decoded.get()) : &ff, decoded ? EXS_LittleEndianExplicit : xfer, flags,
                     decoded ? 0 : static_cast<unsigned long>(frame), 1);
    if (image.getStatus() != EIS_Normal)
    {
        DcmXfer x(xfer);
        if (x.isEncapsulated() && xfer != EXS_RLELossless && xfer != EXS_JPEGLSLossless && xfer != EXS_JPEGLSLossy &&
            x.getJPEGProcess8Bit() == 0 && !decoded)
            return Compression(x.getXferID()) + L" is not supported";
        return Widen(DicomImage::getString(image.getStatus()));
    }
    const int w = static_cast<int>(image.getWidth()), h = static_cast<int>(image.getHeight());
    if (w <= 0 || h <= 0) return L"it has no pixels";
    const size_t n = static_cast<size_t>(w) * h;
    s.width = w;
    s.height = h;
    s.mono = image.isMonochrome() != 0;
    if (!s.mono)
    {
        const Uint8* out = static_cast<const Uint8*>(image.getOutputData(8, 0, 0));
        if (!out) return L"its pixels could not be rendered";
        s.rgb.resize(n);
        for (size_t i = 0; i < n; i++) s.rgb[i] = (static_cast<DWORD>(out[3 * i]) << 16) | (out[3 * i + 1] << 8) | out[3 * i + 2];
        return std::wstring();
    }
    s.inverse = image.getPhotometricInterpretation() == EPI_Monochrome1;
    const DiPixel* inter = image.getInterData();
    if (!inter || !inter->getData() || inter->getCount() < n) return L"its pixels could not be read";
    s.values.resize(n);
    auto copy = [&](auto* p) { for (size_t i = 0; i < n; i++) s.values[i] = static_cast<float>(p[i]); };
    const void* d = inter->getData();
    switch (inter->getRepresentation())
    {
    case EPR_Uint8: copy(static_cast<const Uint8*>(d)); break;
    case EPR_Sint8: copy(static_cast<const Sint8*>(d)); break;
    case EPR_Uint16: copy(static_cast<const Uint16*>(d)); break;
    case EPR_Sint16: copy(static_cast<const Sint16*>(d)); break;
    case EPR_Uint32: copy(static_cast<const Uint32*>(d)); break;
    case EPR_Sint32: copy(static_cast<const Sint32*>(d)); break;
    default: return L"its pixels could not be read";
    }
    // The type of the stored values, from the header: DCMTK gives each frame the smallest one its values
    // fit in, and another frame of the same file can need a larger one
    Uint16 allocated = 0, representation = 0;
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    ds->findAndGetUint16(DCM_PixelRepresentation, representation);
    s.bits = allocated <= 8 ? 8 : allocated <= 16 ? 16 : 32;
    if (representation == 1) s.bits = -s.bits;
    return std::wstring();
}

// 8-bit pixels, 0xRRGGBB, top-down
struct Picture
{
    int width = 0, height = 0;
    std::vector<DWORD> px;
};

static bool IsGray(const Picture& p)
{
    for (DWORD c : p.px)
        if (((c >> 16) & 0xFF) != (c & 0xFF) || ((c >> 8) & 0xFF) != (c & 0xFF)) return false;
    return true;
}

// The box of some planes (the same size) where they are not background: the pixels farther than `radius`
// from any background one (value <= background), one more around - crop( I , 1 , ~imdilate( I == 0 ,
// ball(R) ) ) of DICOMstackCollect.m, by a chamfer distance (3 a step, 4 a diagonal). Inside `within`;
// all of it if there is no such pixel.
static RECT ContentBox(const std::vector<const float*>& planes, int w, const RECT& within, float background, double radius)
{
    const int bw = within.right - within.left, bh = within.bottom - within.top;
    if (bw <= 0 || bh <= 0) return within;
    std::vector<int> d(static_cast<size_t>(bw) * bh);
    const int big = 1 << 28, limit = static_cast<int>(radius * 3);
    int left = bw, top = bh, right = -1, bottom = -1;
    for (const float* p : planes)
    {
        for (int y = 0; y < bh; y++)
        {
            const float* row = p + static_cast<size_t>(y + within.top) * w + within.left;
            for (int x = 0; x < bw; x++) d[static_cast<size_t>(y) * bw + x] = row[x] <= background ? 0 : big;
        }
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
            {
                int& v = d[static_cast<size_t>(y) * bw + x];
                if (!v) continue;
                if (x > 0) v = (std::min)(v, d[static_cast<size_t>(y) * bw + x - 1] + 3);
                if (y > 0)
                {
                    const int* up = &d[static_cast<size_t>(y - 1) * bw];
                    v = (std::min)(v, up[x] + 3);
                    if (x > 0) v = (std::min)(v, up[x - 1] + 4);
                    if (x + 1 < bw) v = (std::min)(v, up[x + 1] + 4);
                }
            }
        for (int y = bh - 1; y >= 0; y--)
            for (int x = bw - 1; x >= 0; x--)
            {
                int& v = d[static_cast<size_t>(y) * bw + x];
                if (!v) continue;
                if (x + 1 < bw) v = (std::min)(v, d[static_cast<size_t>(y) * bw + x + 1] + 3);
                if (y + 1 < bh)
                {
                    const int* down = &d[static_cast<size_t>(y + 1) * bw];
                    v = (std::min)(v, down[x] + 3);
                    if (x > 0) v = (std::min)(v, down[x - 1] + 4);
                    if (x + 1 < bw) v = (std::min)(v, down[x + 1] + 4);
                }
                if (v > limit)
                {
                    left = (std::min)(left, x);
                    right = (std::max)(right, x);
                    top = (std::min)(top, y);
                    bottom = (std::max)(bottom, y);
                }
            }
    }
    if (right < 0) return within;
    return { within.left + (std::max)(0, left - 1), within.top + (std::max)(0, top - 1),
             within.left + (std::min)(bw, right + 2), within.top + (std::min)(bh, bottom + 2) };
}

// Two percentiles of the values of some planes inside a box (from a sample of 2 million at most)
static void Percentiles(const std::vector<const float*>& planes, int w, const RECT& box, double p, double q, double& a, double& b)
{
    const size_t area = static_cast<size_t>(box.right - box.left) * (box.bottom - box.top);
    const size_t total = area * planes.size(), step = total > 2000000 ? total / 2000000 + 1 : 1;
    std::vector<float> v;
    v.reserve(total / step + 1);
    size_t k = 0;
    for (const float* plane : planes)
        for (int y = box.top; y < box.bottom; y++)
            for (int x = box.left; x < box.right; x++, k++)
                if (k % step == 0) v.push_back(plane[static_cast<size_t>(y) * w + x]);
    if (v.empty()) { a = 0; b = 1; return; }
    auto at = [&](double share) {
        const size_t i = static_cast<size_t>(share * (v.size() - 1) + 0.5);
        std::nth_element(v.begin(), v.begin() + i, v.end());
        return static_cast<double>(v[i]);
    };
    a = at(p);
    b = at(q);
}

// A box of a slice as a picture of tw x th, each pixel the mean of those it covers; monochrome, the
// values from lo to hi as black to white
static Picture Shrink(const Slice& s, const RECT& box, double lo, double hi, int tw, int th)
{
    Picture p;
    p.width = tw;
    p.height = th;
    p.px.resize(static_cast<size_t>(tw) * th);
    const int left = box.left, top = box.top, bw = box.right - box.left, bh = box.bottom - box.top;
    const double scale = hi > lo ? 255.0 / (hi - lo) : 0;
    for (int y = 0; y < th; y++)
    {
        const int y0 = top + static_cast<int>(static_cast<long long>(y) * bh / th);
        const int y1 = (std::max)(y0 + 1, top + static_cast<int>(static_cast<long long>(y + 1) * bh / th));
        for (int x = 0; x < tw; x++)
        {
            const int x0 = left + static_cast<int>(static_cast<long long>(x) * bw / tw);
            const int x1 = (std::max)(x0 + 1, left + static_cast<int>(static_cast<long long>(x + 1) * bw / tw));
            double r = 0, g = 0, b = 0;
            int k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, k++)
                {
                    const size_t i = static_cast<size_t>(yy) * s.width + xx;
                    if (s.mono)
                    {
                        double v = (s.values[i] - lo) * scale;
                        v = v < 0 ? 0 : v > 255 ? 255 : v;
                        r += s.inverse ? 255 - v : v;
                    }
                    else
                    {
                        const DWORD c = s.rgb[i];
                        r += (c >> 16) & 0xFF;
                        g += (c >> 8) & 0xFF;
                        b += c & 0xFF;
                    }
                }
            DWORD c;
            if (s.mono)
            {
                const DWORD q = static_cast<DWORD>(r / k + 0.5);
                c = (q << 16) | (q << 8) | q;
            }
            else
                c = (static_cast<DWORD>(r / k + 0.5) << 16) | (static_cast<DWORD>(g / k + 0.5) << 8) | static_cast<DWORD>(b / k + 0.5);
            p.px[static_cast<size_t>(y) * tw + x] = c;
        }
    }
    return p;
}

// The size of a box at most `most` pixels on its longer side, its pixels made square
static void FitSize(const Slice& s, const RECT& box, int most, int& tw, int& th)
{
    double w = box.right - box.left, h = box.bottom - box.top;
    if (s.rowSpacing > 0 && s.columnSpacing > 0) h = h * s.rowSpacing / s.columnSpacing;
    const double k = (std::max)(w, h) > most ? most / (std::max)(w, h) : 1;
    tw = (std::max)(1, static_cast<int>(std::lround(w * k)));
    th = (std::max)(1, static_cast<int>(std::lround(h * k)));
}

static void Blit(Picture& to, const Picture& from, int x0, int y0)
{
    for (int y = 0; y < from.height; y++)
        for (int x = 0; x < from.width; x++)
        {
            const int tx = x0 + x, ty = y0 + y;
            if (tx >= 0 && ty >= 0 && tx < to.width && ty < to.height)
                to.px[static_cast<size_t>(ty) * to.width + tx] = from.px[static_cast<size_t>(y) * from.width + x];
        }
}

// The rows and columns of a mosaic of n (DICOMstackCollect.m): 1 x 1 to 1 x 3, 2 x 2, then from 2 x 3 a
// column more and a row more in turns
static void GridOf(int n, int& rows, int& cols)
{
    if (n <= 3) { rows = 1; cols = (std::max)(1, n); return; }
    if (n == 4) { rows = cols = 2; return; }
    rows = 2;
    cols = 3;
    for (int i = 1; rows * cols < n; i++) (i % 2 ? cols : rows)++;
}

// Tiles in a mosaic of `slots` (black where there is none), centered in cells of the largest one
static Picture Mosaic(const std::vector<Picture>& tiles, int slots)
{
    int rows = 1, cols = 1, cw = 1, ch = 1;
    GridOf(slots, rows, cols);
    for (const auto& t : tiles) { cw = (std::max)(cw, t.width); ch = (std::max)(ch, t.height); }
    const int gap = 2;
    Picture m;
    m.width = cols * cw + (cols - 1) * gap;
    m.height = rows * ch + (rows - 1) * gap;
    m.px.assign(static_cast<size_t>(m.width) * m.height, 0x303030);
    for (int k = 0; k < rows * cols; k++)
    {
        const int x = (k % cols) * (cw + gap), y = (k / cols) * (ch + gap);
        for (int yy = 0; yy < ch; yy++)
            std::fill_n(m.px.begin() + static_cast<size_t>(y + yy) * m.width + x, cw, 0);
        if (k < static_cast<int>(tiles.size()))
            Blit(m, tiles[k], x + (cw - tiles[k].width) / 2, y + (ch - tiles[k].height) / 2);
    }
    return m;
}

// The orientations of a series one over the other (DCMthumbnail.m): each one 58 % of the one before
// further down and to the right, with a border of its own color
static Picture Cascade(const std::vector<Picture>& tiles, DWORD background)
{
    std::vector<POINT> at;
    int x = 0, y = 0, w = 1, h = 1;
    for (const auto& t : tiles)
    {
        at.push_back({ x, y });
        w = (std::max)(w, x + t.width);
        h = (std::max)(h, y + t.height);
        x += t.width * 58 / 100;
        y += t.height * 58 / 100;
    }
    Picture c;
    c.width = w;
    c.height = h;
    c.px.assign(static_cast<size_t>(w) * h, background);
    for (size_t k = 0; k < tiles.size(); k++)
    {
        Picture t = tiles[k];
        const DWORD color = Palette[k % PaletteSize];
        for (int yy = 0; yy < t.height; yy++)
            for (int xx = 0; xx < t.width; xx++)
                if (xx < 2 || yy < 2 || xx >= t.width - 2 || yy >= t.height - 2) t.px[static_cast<size_t>(yy) * t.width + xx] = color;
        Blit(c, t, at[k].x, at[k].y);
    }
    return c;
}

// What a thumbnail or an export shows: positions (in their order), each one its images in time order
// An image of a stack: its file, and its frame there (the frames of enhanced files are images of their own)
struct FrameRef
{
    std::wstring path;
    int frame = 0;
};

struct Stack
{
    std::vector<std::vector<FrameRef>> files;
    int frames = 1;                                 // one multi-frame file: its frames are the times
    double delay = 0;                               // ms from time to time (0: not known)
};

struct Thumb
{
    std::vector<Picture> frames;
    int delay = 100;                                // ms from frame to frame
    std::wstring caption, error;                    // what it shows of the stack; why there is nothing
};

// `count` of 0..n-1 from `first` to `last`, evenly (round(linspace), once each)
static std::vector<int> Spread(int first, int last, int count)
{
    std::vector<int> v;
    for (int i = 0; i < count; i++)
    {
        const int k = count == 1 ? (first + last) / 2 : static_cast<int>(std::lround(first + (last - first) * static_cast<double>(i) / (count - 1)));
        if (v.empty() || v.back() != k) v.push_back(k);
    }
    return v;
}

// The thumbnail of a stack, by the recipe of DICOMstackCollect.m: the background cropped, the values from
// their 2nd to their 98th percentile (DICOMstackCollect takes the 10th and the 90th, which burns the
// bright parts white), cropped again; several positions as a mosaic (all of them up to 30, 20 beyond:
// from 5 % to 95 % past 50), several times as frames (all up to 50, 45 up to 100, 60 beyond);
// not more than 300 files read. One position: up to `single` pixels; a mosaic: tiles up to `tile`.
// Middle: only the middle position at the middle time.
static Thumb StackThumb(const Stack& st, int single, int tile, bool middle, const std::atomic<bool>* cancel)
{
    Thumb out;
    const int Z = static_cast<int>(st.files.size());
    if (Z == 0 || st.files[0].empty()) { out.error = L"no files"; return out; }
    const bool multiframe = st.frames > 1 && Z == 1 && st.files[0].size() == 1;
    int T = multiframe ? st.frames : 1;
    if (!multiframe) for (const auto& p : st.files) T = (std::max)(T, static_cast<int>(p.size()));
    std::vector<int> zs, ts;
    if (middle) { zs = { (Z - 1) / 2 }; ts = { (T - 1) / 2 }; }
    else
    {
        zs = Z > 50 ? Spread(static_cast<int>(Z * 0.05), static_cast<int>(Z * 0.95), 20) : Z > 30 ? Spread(0, Z - 1, 20) : Spread(0, Z - 1, Z);
        ts = T > 100 ? Spread(0, T - 1, 60) : T > 50 ? Spread(0, T - 1, 45) : Spread(0, T - 1, T);
        if (zs.size() * ts.size() > 300)
            ts = Spread(0, T - 1, (std::max)(10, static_cast<int>(300 / zs.size())));
        if (zs.size() * ts.size() > 300)
            zs = Spread(0, Z - 1, (std::max)(1, static_cast<int>(300 / ts.size())));
    }
    auto read = [&](int z, int t, Slice& s) {
        const auto& p = st.files[z];
        if (multiframe) return DecodeSlice(p[0].path, t, false, s);
        const FrameRef& r = p[(std::min)(t, static_cast<int>(p.size()) - 1)];
        return DecodeSlice(r.path, r.frame, false, s);
    };
    // The box and the window from some of them: all the positions at the middle time, or a few times
    std::map<std::pair<int, int>, Slice> kept;
    std::wstring why;
    if (zs.size() > 1) for (int z : zs) why = read(z, ts[ts.size() / 2], kept[{ z, ts[ts.size() / 2] }]);
    else for (int t : Spread(0, static_cast<int>(ts.size()) - 1, 8)) why = read(zs[0], ts[t], kept[{ zs[0], ts[t] }]);
    for (auto it = kept.begin(); it != kept.end();)
        it = it->second.width > 0 ? std::next(it) : kept.erase(it);
    if (kept.empty()) { out.error = why.empty() ? L"no image" : why; return out; }
    if (cancel && *cancel) return out;
    const Slice& first = kept.begin()->second;
    const int w = first.width, h = first.height;
    const bool mono = first.mono;
    std::vector<const float*> planes;
    for (const auto& k : kept) if (k.second.width == w && k.second.height == h && k.second.mono && mono) planes.push_back(k.second.values.data());
    RECT box = { 0, 0, w, h };
    double lo = 0, hi = 255;
    if (mono && !planes.empty())
    {
        float least = planes[0][0];
        for (const float* p : planes) for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) least = (std::min)(least, p[i]);
        box = ContentBox(planes, w, box, least, 3.2);
        Percentiles(planes, w, box, 0.02, 0.98, lo, hi);
        if (hi <= lo) hi = lo + 1;
        box = ContentBox(planes, w, box, static_cast<float>(lo), 7.2);
    }
    int tw = 1, th = 1;
    FitSize(first, box, zs.size() > 1 ? tile : single, tw, th);
    for (int t : ts)
    {
        std::vector<Picture> tiles;
        for (int z : zs)
        {
            if (cancel && *cancel) return Thumb();
            Slice fresh;
            auto it = kept.find({ z, t });
            const Slice* s = it != kept.end() ? &it->second : nullptr;
            if (!s && read(z, t, fresh).empty()) s = &fresh;
            if (s && s->width == w && s->height == h && s->mono == mono) tiles.push_back(Shrink(*s, box, lo, hi, tw, th));
            else
            {
                Picture black;
                black.width = tw;
                black.height = th;
                black.px.assign(static_cast<size_t>(tw) * th, 0);
                tiles.push_back(std::move(black));
            }
        }
        out.frames.push_back(tiles.size() == 1 ? std::move(tiles[0]) : Mosaic(tiles, static_cast<int>(tiles.size())));
    }
    double delay = st.delay > 0 ? st.delay : first.frameTime > 0 ? first.frameTime : 100;
    delay *= static_cast<double>(T) / ts.size();
    out.delay = static_cast<int>((std::max)(20.0, (std::min)(500.0, delay)));
    if (!middle)
    {
        if (Z > 1) out.caption = (static_cast<int>(zs.size()) < Z ? std::to_wstring(zs.size()) + L" of " : L"") + std::to_wstring(Z) + L" positions";
        if (T > 1)
            out.caption += (out.caption.empty() ? L"" : L", ") + (static_cast<int>(ts.size()) < T ? std::to_wstring(ts.size()) + L" of " : L"") +
                           std::to_wstring(T) + (multiframe ? L" frames" : L" times");
    }
    return out;
}

// ----- writing: PNG and animated GIF (by WIC), NIfTI

// Gray pictures as 8-bit gray, color ones as 24-bit BGR, rows of a multiple of 4 bytes
static void Pack(const Picture& p, bool gray, std::vector<BYTE>& bytes, UINT& stride)
{
    stride = ((gray ? 1 : 3) * p.width + 3) & ~3;
    bytes.assign(static_cast<size_t>(stride) * p.height, 0);
    for (int y = 0; y < p.height; y++)
        for (int x = 0; x < p.width; x++)
        {
            const DWORD c = p.px[static_cast<size_t>(y) * p.width + x];
            BYTE* b = &bytes[static_cast<size_t>(y) * stride + (gray ? x : 3 * x)];
            if (gray) b[0] = static_cast<BYTE>(c & 0xFF);
            else { b[0] = static_cast<BYTE>(c & 0xFF); b[1] = static_cast<BYTE>((c >> 8) & 0xFF); b[2] = static_cast<BYTE>((c >> 16) & 0xFF); }
        }
}

template <class T> static void Free(T*& p) { if (p) { p->Release(); p = nullptr; } }

static HRESULT EncodePng(IWICImagingFactory* factory, IStream* out, const Picture& p)
{
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    HRESULT hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(out, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize(p.width, p.height);
    const bool gray = IsGray(p);
    WICPixelFormatGUID format = gray ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    std::vector<BYTE> bytes;
    UINT stride = 0;
    Pack(p, gray, bytes, stride);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(p.height, stride, static_cast<UINT>(bytes.size()), bytes.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    Free(props);
    Free(frame);
    Free(encoder);
    return hr;
}

static HRESULT SavePng(IWICImagingFactory* factory, const std::wstring& path, const Picture& p)
{
    IWICStream* stream = nullptr;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = EncodePng(factory, stream, p);
    Free(stream);
    return hr;
}

// An animated GIF that loops for ever: gray frames with a palette of the 256 grays, color ones with a
// palette of their own
class GifWriter
{
public:
    explicit GifWriter(IWICImagingFactory* f) : factory(f) {}
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;
    ~GifWriter() { Free(encoder); Free(stream); }

    HRESULT Open(const std::wstring& path)
    {
        HRESULT hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        return hr;
    }

    HRESULT Add(const Picture& p, int delay)
    {
        if (first)
        {
            // The size of the animation, and its loop (the NETSCAPE2.0 extension: loop count 0)
            first = false;
            IWICMetadataQueryWriter* w = nullptr;
            if (SUCCEEDED(encoder->GetMetadataQueryWriter(&w)))
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UI2;
                v.uiVal = static_cast<USHORT>(p.width);
                w->SetMetadataByName(L"/logscrdesc/Width", &v);
                v.uiVal = static_cast<USHORT>(p.height);
                w->SetMetadataByName(L"/logscrdesc/Height", &v);
                static UCHAR application[] = { 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0' };
                static UCHAR loop[] = { 3, 1, 0, 0, 0 };
                v.vt = VT_UI1 | VT_VECTOR;
                v.caub.cElems = sizeof(application);
                v.caub.pElems = application;
                w->SetMetadataByName(L"/appext/application", &v);
                v.caub.cElems = sizeof(loop);
                v.caub.pElems = loop;
                w->SetMetadataByName(L"/appext/data", &v);
                w->Release();
            }
        }
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* props = nullptr;
        IWICPalette* palette = nullptr;
        IWICBitmap* bitmap = nullptr;
        IWICFormatConverter* converter = nullptr;
        HRESULT hr = encoder->CreateNewFrame(&frame, &props);
        if (SUCCEEDED(hr)) hr = frame->Initialize(props);
        if (SUCCEEDED(hr))
        {
            IWICMetadataQueryWriter* w = nullptr;
            if (SUCCEEDED(frame->GetMetadataQueryWriter(&w)))
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UI2;
                v.uiVal = static_cast<USHORT>((std::max)(2, (delay + 5) / 10));     // (in hundredths)
                w->SetMetadataByName(L"/grctlext/Delay", &v);
                w->Release();
            }
        }
        if (SUCCEEDED(hr)) hr = frame->SetSize(p.width, p.height);
        if (SUCCEEDED(hr)) hr = factory->CreatePalette(&palette);
        WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
        if (SUCCEEDED(hr) && IsGray(p))
        {
            hr = palette->InitializePredefined(WICBitmapPaletteTypeFixedGray256, FALSE);
            if (SUCCEEDED(hr)) hr = frame->SetPalette(palette);
            if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
            std::vector<BYTE> bytes;
            UINT stride = 0;
            Pack(p, true, bytes, stride);
            if (SUCCEEDED(hr)) hr = frame->WritePixels(p.height, stride, static_cast<UINT>(bytes.size()), bytes.data());
        }
        else if (SUCCEEDED(hr))
        {
            hr = factory->CreateBitmapFromMemory(p.width, p.height, GUID_WICPixelFormat32bppBGR, p.width * 4,
                                                 static_cast<UINT>(p.px.size() * 4), reinterpret_cast<BYTE*>(const_cast<DWORD*>(p.px.data())), &bitmap);
            if (SUCCEEDED(hr)) hr = palette->InitializeFromBitmap(bitmap, 256, FALSE);
            if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
            if (SUCCEEDED(hr)) hr = converter->Initialize(bitmap, GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion, palette, 0,
                                                          WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) hr = frame->SetPalette(palette);
            if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
            if (SUCCEEDED(hr)) hr = frame->WriteSource(converter, nullptr);
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        Free(converter);
        Free(bitmap);
        Free(palette);
        Free(props);
        Free(frame);
        return hr;
    }

    HRESULT Close()
    {
        HRESULT hr = encoder ? encoder->Commit() : E_UNEXPECTED;
        Free(encoder);
        Free(stream);
        return hr;
    }

private:
    IWICImagingFactory* factory;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    bool first = true;
};

// The NIfTI-1 header (348 bytes), the format of a single .nii file
#pragma pack(push, 1)
struct NiftiHeader
{
    int sizeof_hdr = 348;
    char data_type[10] = {}, db_name[18] = {};
    int extents = 0;
    short session_error = 0;
    char regular = 'r', dim_info = 0;
    short dim[8] = {};
    float intent_p1 = 0, intent_p2 = 0, intent_p3 = 0;
    short intent_code = 0, datatype = 0, bitpix = 0, slice_start = 0;
    float pixdim[8] = {};
    float vox_offset = 352, scl_slope = 1, scl_inter = 0;
    short slice_end = 0;
    char slice_code = 0, xyzt_units = 0;
    float cal_max = 0, cal_min = 0, slice_duration = 0, toffset = 0;
    int glmax = 0, glmin = 0;
    char descrip[80] = {}, aux_file[24] = {};
    short qform_code = 0, sform_code = 0;
    float quatern_b = 0, quatern_c = 0, quatern_d = 0, qoffset_x = 0, qoffset_y = 0, qoffset_z = 0;
    float srow_x[4] = {}, srow_y[4] = {}, srow_z[4] = {};
    char intent_name[16] = {}, magic[4] = { 'n', '+', '1', 0 };
};
#pragma pack(pop)
static_assert(sizeof(NiftiHeader) == 348, "the NIfTI-1 header is 348 bytes");

// The quaternion of a rotation (its columns: the directions of i, j and k), as nifti_mat44_to_quatern of
// nifti1_io.c; qfac -1 when the columns make a left-handed system (the third one is turned)
static void Quaternion(double R[3][3], double& qb, double& qc, double& qd, double& qfac)
{
    const double det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                       R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
    qfac = 1;
    if (det < 0)
    {
        qfac = -1;
        for (int r = 0; r < 3; r++) R[r][2] = -R[r][2];
    }
    double a = R[0][0] + R[1][1] + R[2][2] + 1, b, c, d;
    if (a > 0.5)
    {
        a = 0.5 * sqrt(a);
        b = 0.25 * (R[2][1] - R[1][2]) / a;
        c = 0.25 * (R[0][2] - R[2][0]) / a;
        d = 0.25 * (R[1][0] - R[0][1]) / a;
    }
    else
    {
        const double xd = 1 + R[0][0] - (R[1][1] + R[2][2]), yd = 1 + R[1][1] - (R[0][0] + R[2][2]), zd = 1 + R[2][2] - (R[0][0] + R[1][1]);
        if (xd > 1.0)
        {
            b = 0.5 * sqrt(xd);
            c = 0.25 * (R[0][1] + R[1][0]) / b;
            d = 0.25 * (R[0][2] + R[2][0]) / b;
            a = 0.25 * (R[2][1] - R[1][2]) / b;
        }
        else if (yd > 1.0)
        {
            c = 0.5 * sqrt(yd);
            b = 0.25 * (R[0][1] + R[1][0]) / c;
            d = 0.25 * (R[1][2] + R[2][1]) / c;
            a = 0.25 * (R[0][2] - R[2][0]) / c;
        }
        else
        {
            d = 0.5 * sqrt(zd);
            b = 0.25 * (R[0][2] + R[2][0]) / d;
            c = 0.25 * (R[1][2] + R[2][1]) / d;
            a = 0.25 * (R[1][0] - R[0][1]) / d;
        }
        if (a < 0) { b = -b; c = -c; d = -d; }
    }
    qb = b;
    qc = c;
    qd = d;
}

// One file to write: a PNG, an animated GIF or a NIfTI volume
struct ExportUnit
{
    enum Kind { Png, Gif, Nifti } kind = Png;
    std::wstring out;
    Stack stack;
    bool overTime = false;                          // a GIF: through the times (a cine), or through the positions
    // NIfTI: where the positions are (DICOM, mm), the plane, the spacings
    bool placed = false, sameRescale = true;
    double orientation[6] = {};
    std::vector<std::array<double, 3>> positions;
    double thickness = 0, timeStep = 0;
    std::string description;
};

// The pictures of a unit: the window dragged in the preview, that of the header, or that of most of
// the values (0.5 % to 99.5 %) of up to 8 of its images
static std::wstring WritePictures(IWICImagingFactory* factory, const ExportUnit& u, bool dragged, double center, double width,
                                  const std::atomic<bool>& cancel, std::atomic<long>& done, bool& wrote)
{
    const Stack& st = u.stack;
    std::vector<std::pair<std::wstring, int>> order;         // (file, frame)
    if (u.kind == ExportUnit::Png) order.push_back({ st.files[0][0].path, st.files[0][0].frame });
    else if (st.frames > 1 && st.files.size() == 1 && st.files[0].size() == 1)
        for (int k = 0; k < st.frames; k++) order.push_back({ st.files[0][0].path, k });
    else if (u.overTime) for (const auto& f : st.files[0]) order.push_back({ f.path, f.frame });
    else for (const auto& p : st.files) order.push_back({ p[0].path, p[0].frame });
    Slice first;
    std::wstring why = DecodeSlice(order[0].first, order[0].second, false, first);
    if (!why.empty()) return why;
    double lo = 0, hi = 255;
    if (first.mono)
    {
        if (dragged) { lo = center - width / 2; hi = center + width / 2; }
        else if (first.window > 0) { lo = first.center - first.window / 2; hi = first.center + first.window / 2; }
        else
        {
            std::vector<Slice> some(1, first);
            for (int k : Spread(1, static_cast<int>(order.size()) - 1, (std::min)(7, static_cast<int>(order.size()) - 1)))
            {
                Slice s;
                if (k > 0 && DecodeSlice(order[k].first, order[k].second, false, s).empty() && s.width == first.width && s.height == first.height && s.mono)
                    some.push_back(std::move(s));
            }
            std::vector<const float*> planes;
            for (const auto& s : some) planes.push_back(s.values.data());
            Percentiles(planes, first.width, { 0, 0, first.width, first.height }, 0.005, 0.995, lo, hi);
        }
        if (hi <= lo) hi = lo + 1;
    }
    const RECT all = { 0, 0, first.width, first.height };
    if (u.kind == ExportUnit::Png)
    {
        const HRESULT hr = SavePng(factory, u.out, Shrink(first, all, lo, hi, first.width, first.height));
        done++;
        wrote = SUCCEEDED(hr);
        return wrote ? std::wstring() : L"it could not be written";
    }
    double delay = u.overTime ? (st.delay > 0 ? st.delay : first.frameTime > 0 ? first.frameTime : 100) : 80;
    GifWriter gif(factory);
    HRESULT hr = gif.Open(u.out);
    long skipped = 0;
    for (size_t k = 0; k < order.size() && SUCCEEDED(hr); k++)
    {
        if (cancel) return L"stopped";
        Slice s;
        const Slice* use = &first;
        if (k > 0) use = DecodeSlice(order[k].first, order[k].second, false, s).empty() ? &s : nullptr;
        if (use && use->width == first.width && use->height == first.height && use->mono == first.mono)
            hr = gif.Add(Shrink(*use, all, lo, hi, first.width, first.height), static_cast<int>(delay));
        else skipped++;
        done++;
    }
    if (SUCCEEDED(hr)) hr = gif.Close();
    if (FAILED(hr)) return L"it could not be written";
    wrote = true;
    return skipped ? std::to_wstring(skipped) + L" of its images could not be read (left out)" : std::wstring();
}

// A NIfTI-1 volume (.nii): columns, rows, positions and times, the values as stored with their rescale
// in the header (or as 32-bit floats, rescaled, when the files do not share it); RGB as 24 bits. Its
// place: the sform (and the qform) from ImagePositionPatient and ImageOrientationPatient, in RAS.
static std::wstring WriteNifti(const ExportUnit& u, const std::atomic<bool>& cancel, std::atomic<long>& done, bool& wrote)
{
    const Stack& st = u.stack;
    const int Z = static_cast<int>(st.files.size());
    const bool multiframe = st.frames > 1 && Z == 1 && st.files[0].size() == 1;
    const int T = multiframe ? st.frames : static_cast<int>(st.files[0].size());
    auto path = [&](int z, int t) { return multiframe ? FrameRef{ st.files[0][0].path, t } : st.files[z][t]; };
    Slice first;
    std::wstring why = DecodeSlice(path(0, 0).path, path(0, 0).frame, true, first);
    if (!why.empty()) return why;
    NiftiHeader hd;
    const bool asFloat = first.mono && (!u.sameRescale || first.isFloat);     // (decimal pixels: as they are)
    if (!first.mono) { hd.datatype = 128; hd.bitpix = 24; }
    else if (asFloat) { hd.datatype = 16; hd.bitpix = 32; }
    else
        switch (first.bits)
        {
        case 8: hd.datatype = 2; hd.bitpix = 8; break;
        case -8: hd.datatype = 256; hd.bitpix = 8; break;
        case 16: hd.datatype = 512; hd.bitpix = 16; break;
        case -16: hd.datatype = 4; hd.bitpix = 16; break;
        case 32: hd.datatype = 768; hd.bitpix = 32; break;
        default: hd.datatype = 8; hd.bitpix = 32; break;
        }
    hd.dim[0] = T > 1 ? 4 : 3;
    hd.dim[1] = static_cast<short>(first.width);
    hd.dim[2] = static_cast<short>(first.height);
    hd.dim[3] = static_cast<short>(Z);
    hd.dim[4] = static_cast<short>(T);
    for (int k = 5; k < 8; k++) hd.dim[k] = 1;
    const double dc = first.columnSpacing > 0 ? first.columnSpacing : 1, dr = first.rowSpacing > 0 ? first.rowSpacing : 1;
    hd.pixdim[0] = 1;
    hd.pixdim[1] = static_cast<float>(dc);
    hd.pixdim[2] = static_cast<float>(dr);
    hd.pixdim[3] = static_cast<float>(u.thickness > 0 ? u.thickness : 1);
    hd.pixdim[4] = static_cast<float>(T > 1 ? (u.timeStep > 0 ? u.timeStep : multiframe && first.frameTime > 0 ? first.frameTime : 1) : 0);
    hd.xyzt_units = static_cast<char>(2 | (T > 1 ? 16 : 0));             // mm, ms
    if (first.mono && !asFloat) { hd.scl_slope = static_cast<float>(first.slope); hd.scl_inter = static_cast<float>(first.intercept); }
    if (first.mono && first.window > 0)
    {
        hd.cal_min = static_cast<float>(first.center - first.window / 2);
        hd.cal_max = static_cast<float>(first.center + first.window / 2);
    }
    strncpy_s(hd.descrip, u.description.c_str(), _TRUNCATE);
    if (u.placed && !u.positions.empty())
    {
        // i along the rows (the first 3 of the orientation), j down the columns, k from position to position;
        // DICOM is LPS: x and y the other way round in RAS
        const double* o = u.orientation;
        double a[3], b[3], c[3], p[3];
        for (int r = 0; r < 3; r++) { a[r] = o[r] * dc; b[r] = o[3 + r] * dr; p[r] = u.positions[0][r]; }
        double n[3] = { o[1] * o[5] - o[2] * o[4], o[2] * o[3] - o[0] * o[5], o[0] * o[4] - o[1] * o[3] };
        if (Z > 1) for (int r = 0; r < 3; r++) c[r] = (u.positions[Z - 1][r] - u.positions[0][r]) / (Z - 1);
        else for (int r = 0; r < 3; r++) c[r] = n[r] * (u.thickness > 0 ? u.thickness : 1);
        for (double* v : { a, b, c, p }) { v[0] = -v[0]; v[1] = -v[1]; }
        n[0] = -n[0];
        n[1] = -n[1];
        const double step = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (Z > 1 && step > 0) hd.pixdim[3] = static_cast<float>(step);
        for (int k = 0; k < 4; k++)
        {
            const double* col = k == 0 ? a : k == 1 ? b : k == 2 ? c : p;
            hd.srow_x[k] = static_cast<float>(col[0]);
            hd.srow_y[k] = static_cast<float>(col[1]);
            hd.srow_z[k] = static_cast<float>(col[2]);
        }
        hd.sform_code = 1;                          // scanner
        // The qform: the same, its third direction the normal of the plane (towards the positions)
        const double na = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        double sign = c[0] * n[0] + c[1] * n[1] + c[2] * n[2] < 0 ? -1 : 1;
        double R[3][3];
        for (int r = 0; r < 3; r++)
        {
            R[r][0] = a[r] / dc;
            R[r][1] = b[r] / dr;
            R[r][2] = na > 0 ? sign * n[r] / na : 0;
        }
        double qb, qc, qd, qfac;
        Quaternion(R, qb, qc, qd, qfac);
        hd.quatern_b = static_cast<float>(qb);
        hd.quatern_c = static_cast<float>(qc);
        hd.quatern_d = static_cast<float>(qd);
        hd.qoffset_x = static_cast<float>(p[0]);
        hd.qoffset_y = static_cast<float>(p[1]);
        hd.qoffset_z = static_cast<float>(p[2]);
        hd.pixdim[0] = static_cast<float>(qfac);
        hd.qform_code = 1;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, u.out.c_str(), L"wb") || !f) return L"it could not be written";
    const char extension[4] = {};
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && fwrite(extension, 4, 1, f) == 1;
    const size_t n = static_cast<size_t>(first.width) * first.height;
    std::vector<BYTE> bytes(n * hd.bitpix / 8);
    long unread = 0;
    for (int t = 0; t < T && ok; t++)
        for (int z = 0; z < Z && ok; z++)
        {
            if (cancel) { fclose(f); return L"stopped"; }
            Slice s;
            const Slice* use = &first;
            if (t > 0 || z > 0) use = DecodeSlice(path(z, t).path, path(z, t).frame, true, s).empty() ? &s : nullptr;
            if (!use || use->width != first.width || use->height != first.height || use->mono != first.mono) { use = nullptr; unread++; }
            std::fill(bytes.begin(), bytes.end(), static_cast<BYTE>(0));
            for (size_t i = 0; use && i < n; i++)
            {
                if (!first.mono)
                {
                    const DWORD c = use->rgb[i];
                    bytes[3 * i] = static_cast<BYTE>((c >> 16) & 0xFF);
                    bytes[3 * i + 1] = static_cast<BYTE>((c >> 8) & 0xFF);
                    bytes[3 * i + 2] = static_cast<BYTE>(c & 0xFF);
                    continue;
                }
                const double v = use->values[i];
                switch (hd.datatype)
                {
                case 16: { const float x = static_cast<float>(v * use->slope + use->intercept); memcpy(&bytes[4 * i], &x, 4); break; }
                case 2: bytes[i] = static_cast<BYTE>((std::max)(0.0, (std::min)(255.0, v))); break;
                case 256: { const Sint8 x = static_cast<Sint8>((std::max)(-128.0, (std::min)(127.0, v))); memcpy(&bytes[i], &x, 1); break; }
                case 512: { const Uint16 x = static_cast<Uint16>((std::max)(0.0, (std::min)(65535.0, v))); memcpy(&bytes[2 * i], &x, 2); break; }
                case 4: { const Sint16 x = static_cast<Sint16>((std::max)(-32768.0, (std::min)(32767.0, v))); memcpy(&bytes[2 * i], &x, 2); break; }
                case 768: { const Uint32 x = static_cast<Uint32>((std::max)(0.0, (std::min)(4294967295.0, v))); memcpy(&bytes[4 * i], &x, 4); break; }
                default: { const Sint32 x = static_cast<Sint32>((std::max)(-2147483648.0, (std::min)(2147483647.0, v))); memcpy(&bytes[4 * i], &x, 4); break; }
                }
            }
            ok = fwrite(bytes.data(), bytes.size(), 1, f) == 1;
            done++;
        }
    ok = fclose(f) == 0 && ok;
    if (!ok) return L"it could not be written";
    wrote = true;
    return unread ? std::to_wstring(unread) + L" of its images could not be read (zeros there)" : std::wstring();
}

static std::string Base64(const std::vector<BYTE>& in)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3)
    {
        const unsigned v = (in[i] << 16) | (i + 1 < in.size() ? in[i + 1] << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
        out += digits[(v >> 18) & 63];
        out += digits[(v >> 12) & 63];
        out += i + 1 < in.size() ? digits[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? digits[v & 63] : '=';
    }
    return out;
}

static std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// A picture as a PNG in a data: URL
static std::string PngDataUrl(IWICImagingFactory* factory, const Picture& p)
{
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return std::string();
    std::string url;
    if (SUCCEEDED(EncodePng(factory, stream, p)))
    {
        STATSTG stat = {};
        HGLOBAL memory = nullptr;
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stream, &memory)))
        {
            const BYTE* b = static_cast<const BYTE*>(GlobalLock(memory));
            if (b) url = "data:image/png;base64," + Base64(std::vector<BYTE>(b, b + stat.cbSize.QuadPart));
            GlobalUnlock(memory);
        }
    }
    stream->Release();
    return url;
}

// A path that is not there yet: " (2)", " (3)"... before the extension when it is
static std::wstring FreePath(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    const size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L'\\');
    const bool hasDot = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    const std::wstring stem = hasDot ? path.substr(0, dot) : path, ext = hasDot ? path.substr(dot) : L"";
    for (int k = 2;; k++)
    {
        const std::wstring p = stem + L" (" + std::to_wstring(k) + L")" + ext;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    }
}

// The exports and the report, in a thread of their own; the window is told when they are over
struct ExportJob
{
    std::vector<ExportUnit> units;
    bool dragged = false;                           // the window dragged in the preview, for all of them
    double center = 0, width = 0;
    std::wstring report, reportPath;                // a report: its HTML, "{{thumb N}}" for the thumbnail of stacks[N]
    std::vector<Stack> stacks;
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> cancel{ false };
    std::atomic<long> done{ 0 };
    long total = 0;
    std::vector<std::wstring> written, notes;       // (written before the message)
    long withoutImage = 0;                          // orientations left out: no pixels in them
    std::thread thread;
};

static void RunExport(ExportJob* j)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) factory = nullptr;
    if (!j->reportPath.empty())
    {
        // The thumbnails into the report, then the report into its file
        std::vector<std::string> urls(j->stacks.size());
        for (size_t i = 0; i < j->stacks.size() && !j->cancel; i++)
        {
            try
            {
                Thumb t = StackThumb(j->stacks[i], 128, 128, true, &j->cancel);
                if (factory && !t.frames.empty()) urls[i] = PngDataUrl(factory, t.frames[0]);
            }
            catch (...) {}
            j->done++;
        }
        if (!j->cancel)
        {
            std::string html;
            const std::string page = Utf8(j->report);
            size_t at = 0;
            for (;;)
            {
                const size_t k = page.find("{{thumb ", at);
                if (k == std::string::npos) { html += page.substr(at); break; }
                html += page.substr(at, k - at);
                const size_t end = page.find("}}", k);
                const size_t i = static_cast<size_t>(atol(page.c_str() + k + 8));
                html += i < urls.size() ? urls[i] : std::string();
                at = end == std::string::npos ? page.size() : end + 2;
            }
            FILE* f = nullptr;
            if (!_wfopen_s(&f, j->reportPath.c_str(), L"wb") && f)
            {
                const bool ok = fwrite(html.data(), html.size(), 1, f) == 1;
                if (fclose(f) == 0 && ok) j->written.push_back(j->reportPath);
                else j->notes.push_back(L"The report could not be written");
            }
            else j->notes.push_back(L"The report could not be written: " + j->reportPath);
        }
    }
    for (auto& u : j->units)
    {
        if (j->cancel) break;
        const size_t slash = u.out.find_last_of(L'\\');
        if (slash != std::wstring::npos) SHCreateDirectoryExW(nullptr, u.out.substr(0, slash).c_str(), nullptr);
        u.out = FreePath(u.out);
        std::wstring why;
        bool wrote = false;
        try
        {
            if (u.kind == ExportUnit::Nifti) why = WriteNifti(u, j->cancel, j->done, wrote);
            else if (factory) why = WritePictures(factory, u, j->dragged, j->center, j->width, j->cancel, j->done, wrote);
            else why = L"WIC is not available";
        }
        catch (...) { why = L"it could not be written"; wrote = false; }
        // (a file half written is not left behind)
        if (wrote) j->written.push_back(u.out);
        else if (GetFileAttributesW(u.out.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteFileW(u.out.c_str());
        if (j->cancel) break;
        if (!why.empty()) j->notes.push_back(u.out.substr(slash == std::wstring::npos ? 0 : slash + 1) + L": " + why);
    }
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    PostMessageW(j->window, j->message, 0, 0);
}

// The thumbnails of the tree, one at a time in a thread of their own: only the last one asked for (one
// asked before, still being made, is dropped)
struct ThumbAsk
{
    int node = -1;
    unsigned generation = 0;
    bool big = false;                               // for the image panel (larger), not for the floating one
    enum Kind { Single, Cascade, Grid } kind = Single;
    std::vector<Stack> stacks;                      // Single: one; Cascade: the orientations; Grid: the series
    DWORD background = 0;
};

struct ThumbDone
{
    int node = -1;
    unsigned generation = 0;
    bool big = false;
    Thumb thumb;
};

// Two at a time can be waiting: the one of the image panel (first) and the floating one; another one of
// the same kind replaces it, and drops it if it is being made
class ThumbWorker
{
public:
    ThumbWorker(HWND w, UINT m) : window(w), message(m) { thread = std::thread([this] { Run(); }); }
    ThumbWorker(const ThumbWorker&) = delete;
    ThumbWorker& operator=(const ThumbWorker&) = delete;
    ~ThumbWorker()
    {
        {
            std::lock_guard<std::mutex> hold(lock);
            quit = true;
            stale = true;
        }
        wake.notify_one();
        thread.join();
    }
    void Ask(ThumbAsk a)
    {
        {
            std::lock_guard<std::mutex> hold(lock);
            const int k = a.big ? 0 : 1;
            if (running == k) stale = true;
            pending[k] = std::make_unique<ThumbAsk>(std::move(a));
        }
        wake.notify_one();
    }

private:
    HWND window;
    UINT message;
    std::mutex lock;
    std::condition_variable wake;
    std::unique_ptr<ThumbAsk> pending[2];          // big, small
    int running = -1;
    std::atomic<bool> stale{ false };
    bool quit = false;
    std::thread thread;

    void Run()
    {
        for (;;)
        {
            std::unique_ptr<ThumbAsk> a;
            {
                std::unique_lock<std::mutex> hold(lock);
                wake.wait(hold, [&] { return quit || pending[0] || pending[1]; });
                if (quit) return;
                running = pending[0] ? 0 : 1;
                a = std::move(pending[running]);
                stale = false;
            }
            auto done = std::make_unique<ThumbDone>();
            done->node = a->node;
            done->generation = a->generation;
            done->big = a->big;
            try { done->thumb = Make(*a); }
            catch (...) { done->thumb = Thumb(); done->thumb.error = L"it could not be made"; }
            {
                std::lock_guard<std::mutex> hold(lock);
                running = -1;
            }
            if (stale) continue;
            if (PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(done.get()))) done.release();
        }
    }

    Thumb Make(const ThumbAsk& a)
    {
        // (the panel's, about twice the size of the floating one)
        const int single = a.big ? 512 : 256, tile = a.big ? 192 : 128;
        if (a.kind == ThumbAsk::Single) return StackThumb(a.stacks[0], single, tile, false, &stale);
        Thumb out;
        std::vector<Picture> tiles;
        for (const Stack& s : a.stacks)
        {
            if (stale) return out;
            Thumb t = StackThumb(s, a.kind == ThumbAsk::Grid ? (a.big ? 176 : 112) : (a.big ? 288 : 160), tile, true, &stale);
            if (t.frames.empty())
            {
                Picture black;
                black.width = black.height = 48;
                black.px.assign(48 * 48, 0);
                tiles.push_back(std::move(black));
            }
            else tiles.push_back(std::move(t.frames[0]));
        }
        if (tiles.empty()) { out.error = L"no image"; return out; }
        out.frames.push_back(a.kind == ThumbAsk::Cascade ? Cascade(tiles, a.background) : Mosaic(tiles, static_cast<int>(tiles.size())));
        return out;
    }
};

class DicomExplorer
{
    friend struct ExplorerTest;                     // (a test of the gathering, the checks and the exports, without the window)
public:
    DicomExplorer() = default;
    DicomExplorer(const DicomExplorer&) = delete;
    DicomExplorer& operator=(const DicomExplorer&) = delete;
    ~DicomExplorer()
    {
        Stop();
        StopJob();
        worker.reset();
        if (preview) { preview->Unload(); preview->Release(); }
        for (HFONT f : { font, boldFont }) if (f) DeleteObject(f);
        if (icon) DestroyIcon(icon);
        if (smallIcon) DestroyIcon(smallIcon);
    }

    bool Create(const std::wstring& folder, int show, bool report = false)
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
    bool PreTranslate(MSG& m)
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

private:
    static constexpr const wchar_t* Settings = L"Software\\DicomStudyCount\\Explorer";
    static constexpr UINT WM_GATHERED = WM_APP + 20, WM_THUMB = WM_APP + 21, WM_EXPORTED = WM_APP + 22;
    static constexpr UINT_PTR TimerProgress = 1, TimerFilter = 2, TimerFind = 3, TimerFollow = 4, TimerJob = 5;
    static constexpr UINT_PTR TimerFrame = 1;      // (of the floating thumbnail)
    enum { IdPath = 100, IdBrowse, IdReload, IdReport, IdFind, IdCompact, IdIdentity, IdHover, IdFilter, IdNavigator };
    enum class Split { None, Tree, Top, Info };
    struct Row { Uint32 tag; std::wstring value; };     // of the full info
    static constexpr COLORREF ProblemColor = RGB(196, 90, 0);

    // ----- the navigator (DCMexplorer_navigator.m): a window of its own with the images as planes where
    // they are in the patient, in 3-D (orthographic); those of the node chosen in red, and the image itself
    // on its plane when they are one plane. Drag: rotate; right drag (or the wheel): zoom; Shift+drag (or the middle button): move; double click: again.

    // An image in the patient (mm, DICOM's LPS): its corners, at the edges of its pixels
    struct NavPlane { float p[4][3]; };             // top left, top right, bottom left, bottom right

    class Navigator
    {
    public:
        ~Navigator() { if (bitmap) DeleteObject(bitmap); if (font) DeleteObject(font); }

        bool Create(HWND parent)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_module;
            wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
            wc.style = CS_DBLCLKS;
            wc.lpszClassName = L"DicomStudyCountNavigator";
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            wnd = CreateWindowExW(WS_EX_CLIENTEDGE, wc.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, g_module, this);
            return wnd != nullptr;
        }

        bool Visible() const { return wnd && shown; }

        void Show(bool on) { shown = on; if (wnd) ShowWindow(wnd, on ? SW_SHOWNA : SW_HIDE); }

        void Move(const RECT& r) { if (wnd) MoveWindow(wnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE); }

        // What it draws: planes (hot: of the node chosen), and a picture on one of them (-1: none). Fit: the
        // view moved to these planes (another study), its zoom and move undone; the rotation stays.
        void Set(std::vector<NavPlane> p, std::vector<char> h, int on, Picture picture, std::wstring text, std::wstring more, bool fit)
        {
            planes = std::move(p);
            corners.clear();
            hover = -1;
            hoverText.clear();
            hot = std::move(h);
            textured = on;
            caption = std::move(text);
            detail = std::move(more);
            if (bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
            texture = std::move(picture);
            if (textured >= 0 && texture.width > 0)
            {
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = texture.width;
                bi.bmiHeader.biHeight = -texture.height;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                void* bits = nullptr;
                bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bitmap && bits) memcpy(bits, texture.px.data(), texture.px.size() * sizeof(DWORD));
            }
            if (fit)
            {
                zoom = 1;
                panX = panY = 0;
                float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                for (const auto& q : planes)
                    for (const auto& c : q.p)
                        for (int k = 0; k < 3; k++) { lo[k] = (std::min)(lo[k], c[k]); hi[k] = (std::max)(hi[k], c[k]); }
                radius = 1;
                if (!planes.empty())
                {
                    double d = 0;
                    for (int k = 0; k < 3; k++) { center[k] = (lo[k] + hi[k]) / 2; d += (hi[k] - lo[k]) * (hi[k] - lo[k]); }
                    radius = (std::max)(1.0, std::sqrt(d) / 2);
                }
            }
            if (wnd) InvalidateRect(wnd, nullptr, FALSE);
        }

        void Clear() { Set({}, {}, -1, Picture(), L"", L"", true); }

        std::function<void(int)> hovered;           // the mouse went over a plane (its index in Set), or off them (-1)
        static constexpr COLORREF HoverColor = RGB(0, 0, 0);

        void SetHoverText(std::wstring text) { hoverText = std::move(text); if (wnd) InvalidateRect(wnd, nullptr, FALSE); }

    private:
        HWND wnd = nullptr;
        HBITMAP bitmap = nullptr;                   // the picture, for PlgBlt
        HFONT font = nullptr;
        UINT fontDpi = 0;
        std::vector<NavPlane> planes;
        std::vector<char> hot;
        int textured = -1;
        Picture texture;
        std::wstring caption, detail;
        double center[3] = {}, radius = 1, zoom = 1, panX = 0, panY = 0;
        double azimuth = -37.5, elevation = 30;     // (degrees, view(3) of MATLAB)
        POINT last = {}, down = {};                 // (where the mouse is, and where it was pressed)
        bool rotating = false, moving = false, zooming = false;
        bool shown = false;
        std::vector<POINT> corners;                 // of the planes in the window, as last drawn (4 each)
        int hover = -1;                             // the plane under the mouse
        std::wstring hoverText;                     // what it is
        bool tracking = false;                      // (of the mouse leaving)

        int Px(int n) const { const UINT dpi = GetDpiForWindow(wnd); return MulDiv(n, dpi ? dpi : 96, 96); }

        // Where a point of the patient is in the window: viewmtx( az , el ) of MATLAB, its up up
        void Axes(double r[3][3]) const
        {
            const double a = azimuth * 3.14159265358979 / 180, e = elevation * 3.14159265358979 / 180;
            const double m[3][3] = { { cos(a), sin(a), 0 }, { -sin(e) * sin(a), sin(e) * cos(a), cos(e) }, { cos(e) * sin(a), -cos(e) * cos(a), sin(e) } };
            memcpy(r, m, sizeof(m));
        }

        // The patient as blocks (LPS: x to its left, y to its back, z to its head), standing, its nose to the
        // front: head, nose, body, arms, legs. Its sides seen from the view, the farther ones first; each
        // one kept (where it is in the window, which way it faces) for the clicks.
        struct Side { POINT p[4]; double normal[3]; };
        std::vector<Side> figure;                   // as drawn: the last one on top

        void Figure(HDC dc, const double r[3][3], int w, int h)
        {
            struct Block { double c[3], half[3]; COLORREF color; };
            static const Block blocks[] = {
                { { 0, 0, 8.1 }, { 1.0, 1.1, 1.2 }, RGB(236, 196, 164) },        // head
                { { 0, -1.3, 7.9 }, { 0.22, 0.3, 0.35 }, RGB(214, 160, 128) },   // nose
                { { 0, 0, 4.1 }, { 2.0, 1.0, 2.6 }, RGB(90, 140, 210) },         // body
                { { 2.65, 0, 4.4 }, { 0.6, 0.6, 2.3 }, RGB(90, 140, 210) },      // left arm
                { { -2.65, 0, 4.4 }, { 0.6, 0.6, 2.3 }, RGB(90, 140, 210) },     // right arm
                { { 0.95, 0, -1.6 }, { 0.8, 0.8, 3.0 }, RGB(70, 80, 110) },      // left leg
                { { -0.95, 0, -1.6 }, { 0.8, 0.8, 3.0 }, RGB(70, 80, 110) },     // right leg
            };
            const double middle = 2.2;              // (its height from -4.6 to 9.3)
            // (14 high, and as much across when it lies: in a square of a third of the panel, at most 120 px)
            const int size = (std::max)(Px(48), (std::min)(Px(120), (std::min)(w, h) / 3));
            const double k = size / 14.0;
            const POINT o = { Px(8) + size / 2, h - Px(8) - size / 2 };
            auto at = [&](const double v[3]) {
                const double p[3] = { v[0], v[1], v[2] - middle };
                return POINT{ o.x + static_cast<LONG>(std::lround((r[0][0] * p[0] + r[0][1] * p[1] + r[0][2] * p[2]) * k)),
                              o.y - static_cast<LONG>(std::lround((r[1][0] * p[0] + r[1][1] * p[1] + r[1][2] * p[2]) * k)) };
            };
            struct Drawn { Side side; double depth; COLORREF color; };
            std::vector<Drawn> drawn;
            for (const Block& b : blocks)
                for (int axis = 0; axis < 3; axis++)
                    for (int sign : { -1, 1 })
                    {
                        double n[3] = {};
                        n[axis] = sign;
                        const double facing = r[2][0] * n[0] + r[2][1] * n[1] + r[2][2] * n[2];
                        if (facing <= 1e-6) continue;       // (its back)
                        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                        Drawn d = {};
                        double centre[3] = { b.c[0], b.c[1], b.c[2] };
                        centre[axis] += sign * b.half[axis];
                        const int square[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
                        for (int c = 0; c < 4; c++)
                        {
                            double p[3] = { centre[0], centre[1], centre[2] };
                            p[u] += square[c][0] * b.half[u];
                            p[v] += square[c][1] * b.half[v];
                            d.side.p[c] = at(p);
                        }
                        memcpy(d.side.normal, n, sizeof(n));
                        d.depth = r[2][0] * centre[0] + r[2][1] * centre[1] + r[2][2] * (centre[2] - middle);
                        const double light = 0.55 + 0.45 * facing;
                        d.color = RGB(static_cast<int>(GetRValue(b.color) * light), static_cast<int>(GetGValue(b.color) * light),
                                      static_cast<int>(GetBValue(b.color) * light));
                        drawn.push_back(d);
                    }
            std::sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.depth < b.depth; });
            figure.clear();
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            for (const Drawn& d : drawn)
            {
                HBRUSH brush = CreateSolidBrush(d.color);
                HGDIOBJ old = SelectObject(dc, brush);
                Polygon(dc, d.side.p, 4);
                SelectObject(dc, old);
                DeleteObject(brush);
                figure.push_back(d.side);
            }
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            // Its left and its right, at its hands
            SetTextColor(dc, RGB(150, 0, 0));
            for (int s : { 1, -1 })
            {
                const double hand[3] = { s * 3.9, 0, 2.0 };
                const POINT p = at(hand);
                TextOutW(dc, p.x - Px(4), p.y - Px(8), s > 0 ? L"L" : L"R", 1);
            }
        }

        // The side of the figure under a point of the window (nullptr: none)
        const Side* SideAt(POINT pt) const
        {
            for (auto it = figure.rbegin(); it != figure.rend(); ++it)
            {
                bool in = false;
                for (int i = 0, j = 3; i < 4; j = i++)
                {
                    const POINT& a = it->p[i];
                    const POINT& b = it->p[j];
                    if ((a.y > pt.y) != (b.y > pt.y) && pt.x < (b.x - a.x) * static_cast<double>(pt.y - a.y) / (b.y - a.y) + a.x) in = !in;
                }
                if (in) return &*it;
            }
            return nullptr;
        }

        // The view from where a side faces: from the front, from the left, from above...
        void LookFrom(const double n[3])
        {
            elevation = std::asin((std::max)(-1.0, (std::min)(1.0, n[2]))) * 180 / 3.14159265358979;
            if (std::fabs(n[2]) < 0.999) azimuth = std::atan2(n[0], -n[1]) * 180 / 3.14159265358979;
            else azimuth = 0;
            InvalidateRect(wnd, nullptr, FALSE);
        }

        // The plane whose outline is under a point (the nearest one, a few pixels away at most), or whose
        // image is: -1 if none (or the figure is there)
        int PlaneAt(POINT pt) const
        {
            if (SideAt(pt)) return -1;
            const size_t n = (std::min)(planes.size(), corners.size() / 4);
            double best = Px(5) + 0.5;
            int found = -1;
            for (size_t i = 0; i < n; i++)
            {
                const POINT* c = &corners[i * 4];
                static const int edges[4][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 } };
                for (const auto& e : edges)
                {
                    const double ax = c[e[0]].x, ay = c[e[0]].y, bx = c[e[1]].x - ax, by = c[e[1]].y - ay;
                    const double len = bx * bx + by * by;
                    double t = len > 0 ? ((pt.x - ax) * bx + (pt.y - ay) * by) / len : 0;
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    const double d = std::hypot(pt.x - (ax + t * bx), pt.y - (ay + t * by));
                    if (d < best) { best = d; found = static_cast<int>(i); }
                }
            }
            if (found < 0 && bitmap && textured >= 0 && static_cast<size_t>(textured) < n)
            {
                // Inside the image (a parallelogram: the points by the two sides from its first corner)
                const POINT* c = &corners[textured * 4];
                const double ux = c[1].x - c[0].x, uy = c[1].y - c[0].y, vx = c[2].x - c[0].x, vy = c[2].y - c[0].y;
                const double det = ux * vy - uy * vx, px = pt.x - c[0].x, py = pt.y - c[0].y;
                if (std::fabs(det) > 1)
                {
                    const double a = (px * vy - py * vx) / det, b = (ux * py - uy * px) / det;
                    if (a >= 0 && a <= 1 && b >= 0 && b <= 1) found = textured;
                }
            }
            return found;
        }

        void Hover(int i)
        {
            if (i == hover) return;
            hover = i;
            if (i < 0) hoverText.clear();
            InvalidateRect(wnd, nullptr, FALSE);
            if (hovered) hovered(i);
        }

        void Paint(HDC target)
        {
            RECT c;
            GetClientRect(wnd, &c);
            const int w = c.right, h = c.bottom;
            if (w <= 0 || h <= 0) return;
            HDC dc = CreateCompatibleDC(target);
            HBITMAP canvas = CreateCompatibleBitmap(target, w, h);
            HGDIOBJ oldCanvas = SelectObject(dc, canvas);
            FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
            const UINT dpi = GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96;
            if (!font || fontDpi != dpi)
            {
                if (font) DeleteObject(font);
                fontDpi = dpi;
                font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            }
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            double r[3][3];
            Axes(r);
            const double scale = (std::min)(w, h) * 0.5 / radius * zoom;
            auto project = [&](const float* p) {
                const double v[3] = { p[0] - center[0], p[1] - center[1], p[2] - center[2] };
                const double x = r[0][0] * v[0] + r[0][1] * v[1] + r[0][2] * v[2];
                const double y = r[1][0] * v[0] + r[1][1] * v[1] + r[1][2] * v[2];
                return POINT{ static_cast<LONG>(std::lround(w / 2.0 + panX + x * scale)), static_cast<LONG>(std::lround(h / 2.0 + panY - y * scale)) };
            };
            // Where the corners of the planes are (kept, for the mouse over them)
            corners.resize(planes.size() * 4);
            for (size_t i = 0; i < planes.size(); i++)
                for (int k = 0; k < 4; k++) corners[i * 4 + k] = project(planes[i].p[k]);
            // The outlines, the others first, in one call for each color
            auto outlines = [&](int which, COLORREF color, int width) {
                std::vector<POINT> points;
                std::vector<DWORD> counts;
                for (size_t i = 0; i < planes.size(); i++)
                {
                    if (which == 2 ? static_cast<int>(i) != hover : (hot[i] != 0) != (which != 0)) continue;
                    for (int k : { 0, 1, 3, 2, 0 }) points.push_back(corners[i * 4 + k]);
                    counts.push_back(5);
                }
                if (counts.empty()) return;
                HPEN pen = CreatePen(PS_SOLID, width, color);
                HGDIOBJ old = SelectObject(dc, pen);
                PolyPolyline(dc, points.data(), counts.data(), static_cast<DWORD>(counts.size()));
                SelectObject(dc, old);
                DeleteObject(pen);
            };
            outlines(0, RGB(200, 200, 200), 1);
            if (bitmap && textured >= 0 && textured < static_cast<int>(planes.size()))
            {
                const POINT* at = &corners[textured * 4];
                POINT to[3] = { at[0], at[1], at[2] };
                HDC src = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(src, bitmap);
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                PlgBlt(dc, to, src, 0, 0, texture.width, texture.height, nullptr, 0, 0);
                SelectObject(src, old);
                DeleteDC(src);
            }
            outlines(1, RGB(220, 0, 0), Px(2));
            outlines(2, HoverColor, Px(3));             // (the one under the mouse)
            // The patient, at the bottom left, turned as the planes are: a click on a side of it, the view
            // from that side
            Figure(dc, r, w, h);
            // What it is, at the top; how to move it, at the bottom right
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            RECT top = { Px(8), Px(6), w - Px(8), Px(48) };
            const std::wstring head = planes.empty() ? (caption.empty() ? L"Choose a node of the tree" : caption + L"\nNo image here has a position in the patient")
                                                     : caption + (detail.empty() ? L"" : L"\n" + detail);
            RECT fill = top;
            DrawTextW(dc, head.c_str(), -1, &fill, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
            fill.right = (std::min)(fill.right, top.right);
            InflateRect(&fill, Px(3), Px(1));
            FillRect(dc, &fill, GetSysColorBrush(COLOR_WINDOW));
            DrawTextW(dc, head.c_str(), -1, &top, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (hover >= 0 && !hoverText.empty())
            {
                RECT under = { Px(8), fill.bottom + Px(2), w - Px(8), fill.bottom + Px(22) };
                RECT back = under;
                DrawTextW(dc, hoverText.c_str(), -1, &back, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                back.right = (std::min)(back.right, under.right);
                InflateRect(&back, Px(3), Px(1));
                FillRect(dc, &back, GetSysColorBrush(COLOR_WINDOW));
                SetTextColor(dc, HoverColor);
                DrawTextW(dc, hoverText.c_str(), -1, &under, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            RECT bottom = { Px(16) + (std::max)(Px(48), (std::min)(Px(120), (std::min)(w, h) / 3)), h - Px(24), w - Px(8), h - Px(4) };
            DrawTextW(dc, L"drag: rotate   click the figure: from that side   right drag or wheel: zoom   Shift+drag: move", -1, &bottom, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
            BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldFont);
            SelectObject(dc, oldCanvas);
            DeleteObject(canvas);
            DeleteDC(dc);
        }

        static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto self = reinterpret_cast<Navigator*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (msg)
            {
            case WM_NCCREATE:
                self = static_cast<Navigator*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                self->wnd = hwnd;
                break;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                if (self) self->Paint(dc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_SIZE:
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                if (!self) break;
                // Left: rotate (with Shift: move); right: zoom (up: in); middle: move
                self->moving = msg == WM_MBUTTONDOWN || (msg == WM_LBUTTONDOWN && (wp & MK_SHIFT));
                self->rotating = msg == WM_LBUTTONDOWN && !self->moving;
                self->zooming = msg == WM_RBUTTONDOWN;
                self->last = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                SetFocus(hwnd);
                self->down = self->last;
                self->Hover(-1);
                SetCapture(hwnd);
                return 0;
            case WM_MOUSEMOVE:
                if (self && (self->rotating || self->moving || self->zooming))
                {
                    const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), dx = x - self->last.x, dy = y - self->last.y;
                    self->last = { x, y };
                    if (self->rotating)
                    {
                        self->azimuth = std::fmod(self->azimuth - dx * 0.5, 360.0);
                        self->elevation = (std::max)(-90.0, (std::min)(90.0, self->elevation + dy * 0.5));
                    }
                    else if (self->zooming) self->zoom = (std::max)(0.05, (std::min)(200.0, self->zoom * std::pow(1.01, -dy)));
                    else { self->panX += dx; self->panY += dy; }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else if (self)
                {
                    if (!self->tracking)
                    {
                        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
                        self->tracking = TrackMouseEvent(&t) != FALSE;
                    }
                    self->Hover(self->PlaneAt({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }));
                }
                return 0;
            case WM_MOUSELEAVE:
                if (self) { self->tracking = false; self->Hover(-1); }
                return 0;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                if (self && (self->rotating || self->moving || self->zooming))
                {
                    ReleaseCapture();
                    // A click (not a drag) on the figure: the view from that side of it
                    const POINT up = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                    const Side* side = self->SideAt(self->down);
                    if (msg == WM_LBUTTONUP && side && std::abs(up.x - self->down.x) + std::abs(up.y - self->down.y) <= self->Px(3))
                    {
                        self->LookFrom(side->normal);
                    }
                }
                return 0;
            case WM_SETCURSOR:
                if (self && LOWORD(lp) == HTCLIENT)
                {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd, &pt);
                    SetCursor(LoadCursor(nullptr, self->SideAt(pt) ? IDC_HAND : IDC_SIZEALL));
                    return TRUE;
                }
                break;
            case WM_CAPTURECHANGED:
                if (self) self->rotating = self->moving = self->zooming = false;
                return 0;
            case WM_LBUTTONDBLCLK:
                if (!self) break;
                self->azimuth = -37.5;
                self->elevation = 30;
                self->zoom = 1;
                self->panX = self->panY = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_MOUSEWHEEL:
                if (!self) break;
                self->zoom = (std::max)(0.05, (std::min)(200.0, self->zoom * std::pow(1.15, GET_WHEEL_DELTA_WPARAM(wp) / 120.0)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_NCDESTROY:
                if (self) self->wnd = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    };

    HWND wnd = nullptr, pathBox = nullptr, browse = nullptr, reload = nullptr, reportButton = nullptr, findLabel = nullptr, findBox = nullptr;
    HWND compactBox = nullptr, identityBox = nullptr, hoverBox = nullptr, navigatorBox = nullptr, tree = nullptr, info = nullptr, filterLabel = nullptr, filterBox = nullptr;
    HWND full = nullptr, status = nullptr;
    int firstProblemRow = INT_MAX;                  // of the info: the rows of the problems, from this one
    // The floating thumbnail of the node under the mouse in the tree
    HWND popup = nullptr;
    bool hoverPreview = true, hoverArmed = false;
    HTREEITEM hoverItem = nullptr;
    int popupNode = -1, popupFrame = 0;
    std::unique_ptr<ThumbWorker> worker;
    std::unordered_map<int, std::shared_ptr<Thumb>> thumbs;     // by node, for this gathering
    std::unordered_map<int, std::shared_ptr<Thumb>> posters;    // the larger ones of the image panel
    // The navigator, and the planes of the images: each one once (the images of a position over each other)
    Navigator navigator;
    std::vector<NavPlane> navPlanes;
    std::vector<int> filePlane;                     // the plane of each file (-1: none, without a position)
    bool planesMade = false;
    int navigated = -1;                             // the planes the navigator shows: of this node (a study, mostly)
    std::vector<int> navScene;                      // the plane of each one it draws
    std::vector<int> lit;                           // the nodes of the plane under the mouse there, marked in the tree
    unsigned generation = 0;
    // An export or a report being written
    std::unique_ptr<ExportJob> job;
    bool reportOnLoad = false;                      // the report of the folder once it is read (rundll32 ...,Explore /report <folder>)
    HFONT font = nullptr, boldFont = nullptr;
    HICON icon = nullptr, smallIcon = nullptr;
    PreviewHandler* preview = nullptr;
    RECT imageRect = {}, treeSplit = {}, topSplit = {}, infoSplit = {};
    Split split = Split::None;
    int treeWidth = 430, infoWidth = 380, topShare = 560;  // (widths at 96 dpi; the share in thousandths)

    std::wstring root, summary;                     // the folder, and what was found in it
    std::unique_ptr<Gathering> data;
    bool ready = false, compact = true, hideIdentity = true;
    std::vector<GatherNode> nodes;
    std::vector<int> fileNode;                      // the image node of each file
    std::vector<int> rank;                          // the place of each file in the order of the images
    int selected = -1;                              // the node chosen
    int sliderNode = -1;                            // the node whose files the slider goes through
    bool following = false;                         // the tree selection moves with the slider
    std::wstring findText, filterText;
    std::wregex findRegex, filterRegex;
    bool findIsRegex = false, filterIsRegex = false;

    // ----- settings

    static DWORD ReadSetting(const wchar_t* name, DWORD fallback)
    {
        DWORD value = 0, size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, Settings, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
    }
    static void WriteSetting(const wchar_t* name, DWORD value)
    {
        RegSetKeyValueW(HKEY_CURRENT_USER, Settings, name, REG_DWORD, &value, sizeof(value));
    }
    static std::wstring ReadText(const wchar_t* name)
    {
        wchar_t buffer[2048] = {};
        DWORD size = sizeof(buffer);
        return RegGetValueW(HKEY_CURRENT_USER, Settings, name, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS ? buffer : L"";
    }
    void SaveSettings()
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
    }

    int Px(int n) const
    {
        const UINT dpi = wnd ? GetDpiForWindow(wnd) : 96;
        return MulDiv(n, dpi ? dpi : 96, 96);
    }

    static std::wstring TextOf(HWND h)
    {
        std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)) + 1, L'\0');
        s.resize(GetWindowTextW(h, &s[0], static_cast<int>(s.size())));
        return s;
    }

    // ----- the window

    void CreateControls()
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
        identityBox = make(0, L"BUTTON", L"Hide patient identity", WS_TABSTOP | BS_AUTOCHECKBOX, IdIdentity);
        // (no tooltips of its own: the floating thumbnail writes the line of the node)
        tree = make(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                    TVS_SHOWSELALWAYS | TVS_NOTOOLTIPS, 0);
        SetWindowSubclass(tree, TreeProc, 1, reinterpret_cast<DWORD_PTR>(this));
        popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                L"DicomStudyCountThumb", L"", WS_POPUP | WS_BORDER, 0, 0, 10, 10, wnd, nullptr, g_module, this);
        if (popup) SetLayeredWindowAttributes(popup, 0, 255, LWA_ALPHA);
        worker = std::make_unique<ThumbWorker>(wnd, WM_THUMB);
        info = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER, 0);
        filterLabel = make(0, L"STATIC", L"Full info", SS_LEFT | SS_CENTERIMAGE, 0);
        filterBox = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IdFilter);
        full = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0);
        status = make(0, STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, 0);
        SendMessageW(findBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"text or regex; Enter / F3: next"));
        SendMessageW(filterBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter: tag, name or value (text or regex)"));
        Button_SetCheck(compactBox, compact ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(identityBox, hideIdentity ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(hoverBox, hoverPreview ? BST_CHECKED : BST_UNCHECKED);
        navigator.hovered = [this](int i) { Light(i); };
        if (navigator.Create(wnd) && ReadSetting(L"Navigator", 0)) { Button_SetCheck(navigatorBox, BST_CHECKED); navigator.Show(true); }
        SetWindowTheme(tree, L"Explorer", nullptr);
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
        column(full, 1, L"Attribute", 230);
        column(full, 2, L"Value", 600);
        ListView_EnableGroupView(full, TRUE);
        Refont();
        // The image: the preview, alone
        preview = new (std::nothrow) PreviewHandler();
        if (preview)
        {
            preview->moved = [this](int i) { Followed(i); };
            preview->Host(wnd, imageRect);
            preview->ShowFiles({}, -1);
        }
    }

    void Refont()
    {
        for (HFONT* f : { &font, &boldFont }) if (*f) { DeleteObject(*f); *f = nullptr; }
        const int points9 = -MulDiv(9, GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96, 72);
        font = CreateFontW(points9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
        boldFont = CreateFontW(points9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
        for (HWND h : { pathBox, browse, reload, reportButton, findLabel, findBox, compactBox, hoverBox, navigatorBox, identityBox, tree, info, filterLabel,
                        filterBox, full, status })
            if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (filterLabel) SendMessageW(filterLabel, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont), TRUE);
    }

    // The folder and the options at the top, the tree at the left; at the right the node and its image,
    // and under them its full info; the status bar at the bottom. The bars between them move.
    void Layout()
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
        put(compactBox, right - Px(80), Px(80));
        right -= Px(80) + gap * 2;
        put(findBox, right - Px(200), Px(200));
        right -= Px(200) + Px(4);
        put(findLabel, right - Px(34), Px(34));
        right -= Px(34) + gap * 2;
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
        MoveWindow(tree, m, top, tw, bottom - top, TRUE);
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
        MoveWindow(filterLabel, x0, fy, Px(70), line, TRUE);
        MoveWindow(filterBox, x0 + Px(70), fy, (std::min)(Px(420), (navigator.Visible() ? (x1 - x0 - gap) / 2 : x1 - x0) - Px(70)), line, TRUE);
        // (the navigator, if shown, at the right of the full info: half of it each)
        const bool navigating = navigator.Visible();
        const int fw = navigating ? (x1 - x0 - gap) / 2 : x1 - x0;
        MoveWindow(full, x0, fy + line + Px(4), fw, bottom - fy - line - Px(4), TRUE);
        if (navigating) navigator.Move({ x0 + fw + gap, fy, x1, bottom });
        InvalidateRect(wnd, nullptr, TRUE);
    }

    void SetStatus(const std::wstring& text) { if (status) SetWindowTextW(status, text.c_str()); }

    // ----- loading

    void Browse()
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

    void Stop()
    {
        if (data && data->thread.joinable())
        {
            data->cancel = true;
            data->thread.join();
        }
        if (wnd) KillTimer(wnd, TimerProgress);
    }

    void Load(std::wstring folder)
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

    void Clear()
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
        following = false;
        navPlanes.clear();
        filePlane.clear();
        planesMade = false;
        navigated = -1;
        navigator.Clear();
        navScene.clear();
        lit.clear();
        nodes.clear();
        fileNode.clear();
        ListView_DeleteAllItems(info);
        ListView_DeleteAllItems(full);
        ListView_RemoveAllGroups(full);
        if (preview) preview->ShowFiles({}, -1);
    }

    void Progress()
    {
        if (!data) return;
        wchar_t text[160];
        if (data->listing) swprintf_s(text, L"Looking for files: %ld so far...", data->found.load());
        else swprintf_s(text, L"Reading the files: %ld of %ld...", data->done.load(), data->found.load());
        SetStatus(text);
    }

    void Finished()
    {
        KillTimer(wnd, TimerProgress);
        if (!data) return;
        if (data->thread.joinable()) data->thread.join();
        Build();
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

    // ----- the tree of DICOMgather.m

    int Add(Level level, int parent, std::vector<int> files)
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

    // The files grouped by a key, in the order in which the groups appear
    template <class K>
    static std::vector<std::vector<int>> GroupBy(const std::vector<int>& in, K key)
    {
        std::vector<std::vector<int>> groups;
        std::unordered_map<std::string, size_t> at;
        for (int f : in)
        {
            auto r = at.emplace(key(f), groups.size());
            if (r.second) groups.emplace_back();
            groups[r.first->second].push_back(f);
        }
        return groups;
    }

    void Build()
    {
        nodes.clear();
        const std::vector<Gathered>& files = data->files;
        fileNode.assign(files.size(), -1);
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
        rank.assign(files.size(), 0);
        for (size_t i = 0; i < all.size(); i++) rank[all[i]] = static_cast<int>(i);
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
                std::sort(theirs[k].begin(), theirs[k].end(), [&](int a, int b) { return rank[a] < rank[b]; });
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

    void Problem(int n, const std::wstring& flag, const std::wstring& text)
    {
        nodes[n].problems.push_back(text);
        if (std::find(nodes[n].flags.begin(), nodes[n].flags.end(), flag) == nodes[n].flags.end()) nodes[n].flags.push_back(flag);
    }

    // The value of an attribute in a file, if it has it
    const std::wstring* AttributeOf(int f, Uint32 tag) const
    {
        const auto* a = Find(data->files[f].attributes, tag);
        return a ? &data->pool.Text(a->second) : nullptr;
    }

    // Each orientation: gaps in its positions, spacings that are not regular, positions off the line of the
    // normal, positions with another number of images, images without position, pixel spacings that vary.
    // Each series: instance numbers missing. Each file: the same SOP instance UID as another one, shorter
    // than its pixels. The series show what was found under them, the studies and patients how many series.
    void Check()
    {
        const std::vector<Gathered>& files = data->files;
        wchar_t b[400];
        auto mm = [](double v) { wchar_t s[40]; swprintf_s(s, L"%.4g", v); return std::wstring(s); };
        auto fileName = [&](int f) { return files[f].path.substr(files[f].path.find_last_of(L'\\') + 1); };

        // The same SOP instance UID in more than one file (copies)
        std::unordered_map<Uint32, std::vector<int>> uids;
        for (size_t f = 0; f < files.size(); f++)
        {
            if (files[f].frame > 0) continue;       // (the frames of a file share its UID)
            if (const auto* a = Find(files[f].attributes, 0x00080018)) uids[a->second].push_back(static_cast<int>(f));
        }
        for (const auto& [uid, same] : uids)
        {
            if (same.size() < 2) continue;
            for (int f : same)
            {
                std::wstring others;
                for (int g : same) if (g != f && others.size() < 200) others += (others.empty() ? L"" : L", ") + files[g].path;
                Problem(fileNode[f], L"duplicate", L"The same SOP instance UID as " + others);
            }
        }

        for (size_t i = 0; i < nodes.size(); i++)
        {
            const int n = static_cast<int>(i);
            const GatherNode& node = nodes[n];
            if (node.level == Level::Image)
            {
                // Shorter than its pixels (uncompressed only: the size of compressed ones is not known)
                const int f = node.files[0];
                if (files[f].frame > 0) continue;              // (once a file: its first frame)
                const std::wstring* ts = AttributeOf(f, 0x00020010);
                std::wstring syntax = ts ? *ts : L"";
                while (!syntax.empty() && (syntax.back() == L' ' || syntax.back() == 0)) syntax.pop_back();
                if (syntax != L"1.2.840.10008.1.2" && syntax != L"1.2.840.10008.1.2.1" && syntax != L"1.2.840.10008.1.2.2") continue;
                const std::wstring* bits = AttributeOf(f, 0x00280100);
                const std::wstring* samples = AttributeOf(f, 0x00280002);
                const long long need = static_cast<long long>(files[f].rows) * files[f].columns * files[f].frames *
                                       (samples ? (std::max)(1, _wtoi(samples->c_str())) : 1) * (bits ? _wtoi(bits->c_str()) : 0) / 8;
                if (need > 0 && files[f].bytes < need)
                {
                    swprintf_s(b, L"The file is shorter than its pixels: %lld bytes, and its pixels alone need %lld", files[f].bytes, need);
                    Problem(n, L"truncated", b);
                }
                continue;
            }
            if (node.level == Level::Series)
            {
                // Instance numbers missing (when there are few of them, and none repeated): a note, not a
                // problem. In a corpus of 700 000 files almost all of them were the numbering of the scanner
                // (magnitude and phase interleaved, the same phases left out of every slice) or images the
                // operator did not keep; images really missing leave a hole in the positions, checked apart.
                std::vector<long> numbers;
                for (int f : node.files)
                {
                    if (files[f].frame > 0) continue;          // (the frames of a file share its number)
                    const std::string& s = files[f].instance;
                    if (s.empty()) { numbers.clear(); break; }
                    numbers.push_back(atol(s.c_str()));
                }
                std::sort(numbers.begin(), numbers.end());
                if (numbers.size() >= 3 && std::adjacent_find(numbers.begin(), numbers.end()) == numbers.end())
                {
                    const long long missing = static_cast<long long>(numbers.back()) - numbers.front() + 1 - static_cast<long long>(numbers.size());
                    if (missing > 0 && missing <= static_cast<long long>(numbers.size()))
                    {
                        std::wstring which;
                        int listed = 0;
                        for (size_t k = 1; k < numbers.size() && listed < 6; k++)
                            for (long v = numbers[k - 1] + 1; v < numbers[k] && listed < 6; v++, listed++)
                                which += (which.empty() ? L"" : L", ") + std::to_wstring(v);
                        if (missing > listed) which += L"...";
                        // Regular: most runs of the same length, most of them the same distance from the one
                        // before (a few may be off: the same phase dropped from every slice but one)
                        std::vector<std::pair<long, long>> runs;            // (first missing, how many)
                        for (size_t k = 1; k < numbers.size(); k++)
                            if (numbers[k] > numbers[k - 1] + 1) runs.push_back({ numbers[k - 1] + 1, numbers[k] - numbers[k - 1] - 1 });
                        auto mostCommon = [](const std::vector<long>& v, size_t& times) {
                            std::map<long, size_t> c;
                            for (long x : v) c[x]++;
                            long best = 0;
                            times = 0;
                            for (const auto& [x, k] : c) if (k > times) { times = k; best = x; }
                            return best;
                        };
                        std::vector<long> lengths, steps;
                        for (size_t k = 0; k < runs.size(); k++)
                        {
                            lengths.push_back(runs[k].second);
                            if (k) steps.push_back(runs[k].first - runs[k - 1].first);
                        }
                        size_t sameLength = 0, sameStep = 0;
                        const long length = mostCommon(lengths, sameLength), step = mostCommon(steps, sameStep);
                        const bool regular = runs.size() >= 3 && sameLength * 3 >= runs.size() * 2 && sameStep * 3 >= steps.size() * 2;
                        std::wstring why = L"few and scattered: images not kept by the operator, or lost - the positions say whether a slice is missing";
                        if (regular)
                        {
                            wchar_t r[200];
                            swprintf_s(r, L"%s, %ld of every %ld: the numbering of the scanner, or images left out on purpose",
                                       sameStep == steps.size() && sameLength == runs.size() ? L"regular" : L"mostly regular", length, step);
                            why = r;
                        }
                        else if (runs.size() == 1 && runs[0].second > 1) why = L"one block: images not kept, or lost - the positions say whether a slice is missing";
                        else if (runs.size() == 2 && runs[0].second == runs[1].second && runs[0].second > 1)
                            why = L"two blocks of " + std::to_wstring(runs[0].second) + L": images not kept, or lost - the positions say whether a slice is missing";
                        swprintf_s(b, L"Instance numbers missing: %lld between %ld and %ld (%s). %s", missing, numbers.front(), numbers.back(),
                                   which.c_str(), why.c_str());
                        nodes[n].notes.push_back(b);
                    }
                }
                continue;
            }
            if (node.level != Level::Orientation) continue;
            const std::vector<int>& positions = node.children;
            const Gathered& g0 = files[node.files[0]];
            // Images without position, among others that have it
            size_t placed = 0;
            for (int f : node.files) if (files[f].placed) placed++;
            if (placed > 0 && placed < node.files.size())
                Problem(n, L"no position", std::to_wstring(node.files.size() - placed) + L" of its images have no position (ImagePositionPatient)");
            // Pixel spacing
            std::vector<std::wstring> spacings;
            for (int f : node.files)
            {
                const std::wstring* s = AttributeOf(f, 0x00280030);
                if (s && std::find(spacings.begin(), spacings.end(), *s) == spacings.end()) spacings.push_back(*s);
            }
            if (spacings.size() > 1)
                Problem(n, L"pixel spacing varies", L"Pixel spacing varies: " + spacings[0] + L"  |  " + spacings[1] + (spacings.size() > 2 ? L"  |  ..." : L""));
            if (positions.size() < 2) continue;
            // Images per position
            std::map<size_t, size_t> counts;
            for (int p : positions) counts[nodes[p].files.size()]++;
            if (counts.size() > 1)
            {
                size_t most = 0, usual = 0;
                for (const auto& [k, c] : counts) if (c > most) { most = c; usual = k; }
                std::wstring which;
                size_t differ = 0;
                for (int p : positions)
                {
                    const size_t k = nodes[p].files.size();
                    if (k == usual) continue;
                    differ++;
                    if (differ <= 3)
                    {
                        const Gathered& g = files[nodes[p].files[0]];
                        swprintf_s(b, L"%s%s has %zu", which.empty() ? L"" : L", ", g.hasZ ? (L"Z " + mm(g.z)).c_str() : L"one", k);
                        which += b;
                    }
                    Problem(p, std::to_wstring(k) + L" images (most have " + std::to_wstring(usual) + L")",
                            L"This position has " + std::to_wstring(k) + L" images; most positions of its orientation have " + std::to_wstring(usual));
                }
                swprintf_s(b, L"Images per position: most have %zu; %s%s (%zu positions differ)", usual, which.c_str(), differ > 3 ? L"..." : L"", differ);
                Problem(n, L"images per position differ", b);
            }
            // The positions along the normal: gaps, irregular spacing; off its line
            std::vector<const Gathered*> first;
            for (int p : positions) first.push_back(&files[nodes[p].files[0]]);
            if (std::all_of(first.begin(), first.end(), [](const Gathered* g) { return g->placed && g->oriented && g->hasZ; }))
            {
                const double* o = g0.orientation;
                double shift = 0;
                for (const Gathered* g : first)
                {
                    double d[3];
                    for (int r = 0; r < 3; r++) d[r] = g->position[r] - first[0]->position[r];
                    const double u = d[0] * o[0] + d[1] * o[1] + d[2] * o[2], v = d[0] * o[3] + d[1] * o[4] + d[2] * o[5];
                    shift = (std::max)(shift, sqrt(u * u + v * v));
                }
                if (shift > 0.05)
                    Problem(n, L"shifted", L"The positions are not on one line along the normal: up to " + mm(shift) +
                                           L" mm off in the plane of the images (a gantry tilt, or stacks put together)");
                if (positions.size() < 3) continue;
                std::vector<double> steps;
                for (size_t k = 1; k < first.size(); k++) steps.push_back(first[k]->z - first[k - 1]->z);
                std::vector<double> sorted = steps;
                std::sort(sorted.begin(), sorted.end());
                const double usual = sorted[sorted.size() / 2];
                if (!(usual > 1e-3)) continue;
                int gaps = 0;
                long long lost = 0;
                double least = usual, largest = usual;
                std::wstring where;
                for (size_t k = 0; k < steps.size(); k++)
                {
                    if (steps[k] > 1.5 * usual)
                    {
                        if (gaps++ == 0) where = L"from Z " + mm(first[k]->z) + L" to " + mm(first[k + 1]->z) + L" mm";
                        lost += std::llround(steps[k] / usual) - 1;
                    }
                    else { least = (std::min)(least, steps[k]); largest = (std::max)(largest, steps[k]); }
                }
                if (gaps)
                {
                    swprintf_s(b, L"Gap: about %lld position%s missing %s (the usual spacing is %s mm)%s", lost, lost == 1 ? L"" : L"s",
                               where.c_str(), mm(usual).c_str(), gaps > 1 ? (L", and " + std::to_wstring(gaps - 1) + L" more gaps").c_str() : L"");
                    Problem(n, L"gap", b);
                }
                if (largest - least > 0.1 * usual)
                    Problem(n, L"irregular spacing", L"The spacing of its positions is not regular: from " + mm(least) + L" to " + mm(largest) +
                                                     L" mm (usually " + mm(usual) + L" mm)");
            }
        }

        // Up the tree: a series, what was found under it; a study and a patient, how many series
        for (int i = static_cast<int>(nodes.size()) - 1; i > 0; i--)
        {
            GatherNode& node = nodes[i];
            if (node.level == Level::Series)
            {
                std::map<std::wstring, size_t> images;
                std::function<void(int)> walk = [&](int k) {
                    for (int c : nodes[k].children)
                    {
                        if (nodes[c].level == Level::Orientation)
                            for (const auto& f : nodes[c].flags)
                                if (std::find(node.flags.begin(), node.flags.end(), f) == node.flags.end()) node.flags.push_back(f);
                        if (nodes[c].level == Level::Image) for (const auto& f : nodes[c].flags) images[f]++;
                        walk(c);
                    }
                };
                walk(i);
                for (const auto& [flag, k] : images)
                {
                    node.flags.push_back(std::to_wstring(k) + L" " + flag + (flag == L"duplicate" && k != 1 ? L"s" : L""));
                    node.problems.push_back(std::to_wstring(k) + (k == 1 ? L" file " : L" files ") +
                                            (flag == L"duplicate" ? (k == 1 ? L"has the same SOP instance UID as another file (a copy)"
                                                                            : L"have the same SOP instance UID as other files (copies)")
                                                                  : (k == 1 ? L"is shorter than its pixels" : L"are shorter than their pixels")));
                }
            }
            else if ((node.level == Level::Study || node.level == Level::Patient))
            {
                size_t k = 0;
                std::function<void(int)> walk = [&](int c) {
                    if (nodes[c].level == Level::Series) { if (!nodes[c].flags.empty()) k++; return; }
                    for (int d : nodes[c].children) walk(d);
                };
                walk(i);
                // (first who it is: the problems of its series after)
                if (node.level == Level::Patient && node.sharing > 1)
                    Problem(i, L"the same ID, " + std::to_wstring(node.sharing) + L" people",
                            L"Its PatientID is that of " + std::to_wstring(node.sharing) + L" people (other names or birth dates in the files): shown apart. " +
                            L"An export that puts the same placeholder in every PatientID (9999), or files without one");
                if (k) node.flags.push_back(std::to_wstring(k) + L" series with problems");
            }
        }
    }

    // The problems of a node and of those under it, those under it with the line of their node (`most` at
    // most); not those of each position and image (a series counts those of its images)
    std::vector<std::wstring> ProblemsUnder(int n, size_t most) const
    {
        std::vector<std::wstring> out;
        size_t more = 0;
        std::function<void(int)> walk = [&](int k) {
            std::wstring where;
            const bool alone = nodes[k].level == Level::Orientation && nodes[nodes[k].parent].children.size() == 1;
            if (k != n && !alone)
            {
                where = nodes[k].text;
                const size_t mark = where.find(L"   \u26A0");
                if (mark != std::wstring::npos) where.resize(mark);
                where += L":   ";
            }
            for (const auto& p : nodes[k].problems)
            {
                if (out.size() < most) out.push_back(where + p);
                else more++;
            }
            for (int c : nodes[k].children)
                if (nodes[c].level != Level::Position && nodes[c].level != Level::Image) walk(c);
        };
        walk(n);
        if (more) out.push_back(L"... and " + std::to_wstring(more) + L" more");
        return out;
    }

    // A series: its orientations (the same ImageOrientationPatient, rows and columns), their positions
    // (the same ImagePositionPatient, in the order of their Z level) and their images
    void AddSeries(int study, const std::vector<int>& rg)
    {
        const std::vector<Gathered>& files = data->files;
        const int r = Add(Level::Series, study, rg);
        std::vector<std::vector<int>> orientations;
        for (int f : rg)
        {
            const Gathered& g = files[f];
            auto same = [&](const std::vector<int>& o) {
                const Gathered& h = files[o[0]];
                if (g.oriented != h.oriented || g.rows != h.rows || g.columns != h.columns) return false;
                for (int k = 0; k < 6 && g.oriented; k++) if (std::fabs(g.orientation[k] - h.orientation[k]) > 1e-4) return false;
                return true;
            };
            auto it = std::find_if(orientations.begin(), orientations.end(), same);
            if (it == orientations.end()) orientations.push_back({ f });
            else it->push_back(f);
        }
        for (auto& og : orientations)
        {
            const int o = Add(Level::Orientation, r, og);
            // Placed ones by their Z level (the position along the normal), those without a position last
            std::vector<int> sorted = og;
            std::stable_sort(sorted.begin(), sorted.end(), [&](int a, int b) {
                const Gathered &x = files[a], &y = files[b];
                if (x.placed != y.placed) return x.placed;
                if (x.hasZ && y.hasZ && x.z != y.z) return x.z < y.z;
                return false;
            });
            std::vector<std::vector<int>> positions;
            auto same = [&](const Gathered& g, const Gathered& h) {
                if (g.placed != h.placed) return false;
                for (int k = 0; k < 3 && g.placed; k++) if (std::fabs(g.position[k] - h.position[k]) >= 1e-3) return false;
                return true;
            };
            for (int f : sorted)
            {
                const Gathered& g = files[f];
                bool joined = false;
                if (!positions.empty() && same(g, files[positions.back()[0]]))
                {
                    positions.back().push_back(f);
                    joined = true;
                }
                // (a position seen before, not the last one: without Z levels they are not sorted)
                for (size_t k = 0; !joined && g.placed && k < positions.size(); k++)
                    if (same(g, files[positions[k][0]])) { positions[k].push_back(f); joined = true; }
                if (!joined) positions.push_back({ f });
            }
            for (auto& pg : positions)
            {
                // The images of a position in their order (instance number, trigger time...)
                std::sort(pg.begin(), pg.end(), [&](int a, int b) { return rank[a] < rank[b]; });
                const int z = Add(Level::Position, o, pg);
                for (int f : pg) fileNode[f] = Add(Level::Image, z, { f });
            }
        }
    }

    // The files of a node (the frames of an enhanced file are images of their own, but one file)
    size_t FilesOf(int n) const
    {
        size_t k = 0;
        for (int f : nodes[n].files) if (data->files[f].frame <= 0) k++;
        return k;
    }

    // "24 files", or "120 images (1 file)" when frames of enhanced files are among them
    std::wstring HowMany(int n) const
    {
        const size_t files = FilesOf(n), images = nodes[n].files.size();
        const std::wstring f = std::to_wstring(files) + (files == 1 ? L" file" : L" files");
        return files == images ? f : std::to_wstring(images) + L" images (" + f + L")";
    }

    // The line of a node in the tree, and what the checks found in it
    std::wstring Label(int i) const
    {
        std::wstring s = Plain(i);
        const auto& flags = nodes[i].flags;
        for (size_t k = 0; k < flags.size(); k++) s += (k ? L", " : L"   \u26A0 ") + flags[k];
        return s;
    }

    std::wstring Plain(int i) const
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
            return s;
        }
        case Level::Orientation:
            add(Size(i));
            add(L"(" + files(count) + L")");
            return s;
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
    std::wstring Size(int o) const
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
    bool Skipped(int c) const
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

    void ComputeShown()
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

    // The shown children of a node into the tree (when it is opened: a series can have thousands)
    void Insert(int n)
    {
        if (n < 0 || nodes[n].filled) return;
        nodes[n].filled = true;
        HTREEITEM under = n == 0 ? TVI_ROOT : nodes[n].item;
        if (!under) return;
        SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
        for (int c : nodes[n].shown)
        {
            TVINSERTSTRUCTW is = {};
            is.hParent = under;
            is.hInsertAfter = TVI_LAST;
            is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
            is.item.pszText = const_cast<wchar_t*>(nodes[c].text.c_str());
            is.item.lParam = c;
            is.item.cChildren = nodes[c].shown.empty() ? 0 : 1;
            nodes[c].item = TreeView_InsertItem(tree, &is);
        }
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    }

    // The tree item of a node, its parents opened if they were not
    HTREEITEM ItemOf(int n)
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
    void FillTree()
    {
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

    // Compact or not: the tree again, with the same node chosen (or the one shown that holds it)
    void Recompact()
    {
        if (!ready) return;
        int keep = selected;
        ComputeShown();
        FillTree();
        while (keep > 0 && nodes[keep].shownParent < 0) keep = nodes[keep].parent;
        if (HTREEITEM item = ItemOf(keep)) { TreeView_SelectItem(tree, item); TreeView_EnsureVisible(tree, item); }
    }

    // ----- a node chosen

    void Choose(int n)
    {
        if (n <= 0 || n >= static_cast<int>(nodes.size())) return;
        selected = n;
        FillInfo(n);
        FillFull(n);
        Navigate(n);
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
        for (int f : nodes[under].files)
        {
            paths.push_back(data->files[f].path);
            frames.push_back(data->files[f].frame);
        }
        // Not an image: what the floating thumbnail shows (the middle slice, the mosaic, the cine...),
        // larger, until the slider goes to its files
        if (nodes[n].level != Level::Image)
        {
            preview->ShowFiles(paths, -1, frames);
            if (posters.count(n)) ShowPoster(n);
            else AskThumb(n, true);
            return;
        }
        preview->ShowFiles(paths, index, frames);
    }

    void ShowPoster(int n)
    {
        auto it = posters.find(n);
        if (!preview || it == posters.end() || n != sliderNode) return;
        const Thumb& t = *it->second;
        if (t.frames.empty()) { preview->ShowIndex(0); return; }      // (no thumbnail: the first file)
        std::vector<PreviewHandler::PosterFrame> frames;
        for (const Picture& p : t.frames) frames.push_back({ p.width, p.height, p.px });
        std::wstring caption = nodes[n].text;
        if (!t.caption.empty()) caption += L"   -   " + t.caption;
        preview->ShowPoster(std::move(frames), t.delay, caption);
    }

    // ----- the navigator

    // The plane of each image that has a position, orientation, size and pixel spacing; the images in the
    // same plane (of a position: its times, its copies) share it
    void MakePlanes()
    {
        if (planesMade || !data) return;
        planesMade = true;
        const std::vector<Gathered>& files = data->files;
        filePlane.assign(files.size(), -1);
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
            filePlane[f] = static_cast<int>(each.size()) - 1;
        }
        std::sort(keyed.begin(), keyed.end());
        std::vector<int> to(each.size());
        navPlanes.clear();
        for (size_t i = 0; i < keyed.size(); i++)
        {
            if (i == 0 || keyed[i].first != keyed[i - 1].first) navPlanes.push_back(each[keyed[i].second]);
            to[keyed[i].second] = static_cast<int>(navPlanes.size()) - 1;
        }
        for (int& p : filePlane) if (p >= 0) p = to[p];
    }

    // The navigator on a node: the planes of its study (the coordinates of the patient are of a study:
    // another one has others), its own in red; on its plane its image, if it has one plane
    void Navigate(int n)
    {
        if (!navigator.Visible() || !ready || n <= 0 || n >= static_cast<int>(nodes.size())) return;
        MakePlanes();
        int context = n;
        while (context > 0 && nodes[context].level > Level::Study) context = nodes[context].parent;
        if (context <= 0) context = n;
        std::vector<int> mine, around;
        std::vector<char> seen(navPlanes.size(), 0);
        for (int f : nodes[n].files)
            if (filePlane[f] >= 0 && !seen[filePlane[f]]) { seen[filePlane[f]] = 1; mine.push_back(filePlane[f]); }
        for (int f : nodes[context].files)
            if (filePlane[f] >= 0 && !seen[filePlane[f]]) { seen[filePlane[f]] = 2; around.push_back(filePlane[f]); }
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
        navScene.clear();
        for (int p : around) { scene.push_back(navPlanes[p]); hot.push_back(0); navScene.push_back(p); }
        for (int p : mine) { scene.push_back(navPlanes[p]); hot.push_back(1); navScene.push_back(p); }
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
        if (total)
        {
            wchar_t b[200];
            if (context == n) swprintf_s(b, L"%zu plane%s", red, red == 1 ? L"" : L"s");
            else swprintf_s(b, L"%zu plane%s in red, of %zu in its study", red, red == 1 ? L"" : L"s", total);
            more = b;
            if (total > most) more += L" (" + std::to_wstring(most) + L" of them drawn)";
        }
        navigator.Set(std::move(scene), std::move(hot), on, std::move(picture), nodes[n].text, more, context != navigated);
        navigated = context;
    }

    // The mouse over a plane of the navigator (-1: off them): the images of that plane marked in the tree,
    // or the nearest node above them there (when their branch is closed)
    void Light(int i)
    {
        if (lit.empty() && i < 0) return;
        lit.clear();
        if (i >= 0 && i < static_cast<int>(navScene.size()) && navigated > 0)
        {
            const int p = navScene[i];
            size_t images = 0;
            for (int f : nodes[navigated].files)
            {
                if (filePlane[f] != p) continue;
                images++;
                int x = fileNode[f];
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

    // The slider (or the cine) moved: an image chosen in the tree follows it
    void Followed(int i)
    {
        if (sliderNode < 0 || selected < 0 || i < 0 || i >= static_cast<int>(nodes[sliderNode].files.size())) return;
        if (nodes[selected].level != Level::Image || nodes[selected].shownParent != sliderNode) return;
        const int image = fileNode[nodes[sliderNode].files[i]];
        if (image == selected || image < 0 || !nodes[image].item) return;
        following = true;
        TreeView_SelectItem(tree, nodes[image].item);
        following = false;
        selected = image;
        SetTimer(wnd, TimerFollow, 250, nullptr);   // its info when it stops for a moment
    }

    // What the node is: a few values of it, and how many of what are under it
    void FillInfo(int n)
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
    const Attributes& Common(int n)
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

    static const std::pair<Uint32, Uint32>* Find(const Attributes& a, Uint32 tag)
    {
        auto it = std::lower_bound(a.begin(), a.end(), tag, [](const std::pair<Uint32, Uint32>& x, Uint32 t) { return x.first < t; });
        return it != a.end() && it->first == tag ? &*it : nullptr;
    }

    // The value of an attribute in a node: the one of all its files, "(varies)", or nothing
    std::wstring Value(int n, Uint32 tag)
    {
        if (const auto* v = Find(Common(n), tag)) return Shown(tag, data->pool.Text(v->second));
        for (int f : nodes[n].files) if (Find(data->files[f].attributes, tag)) return L"(varies)";
        return L"";
    }

    std::wstring Shown(Uint32 tag, const std::wstring& value) const
    {
        return hideIdentity && IdentifyingTag(tag) && !value.empty() ? L"(hidden)" : value;
    }

    // ----- the full info

    static bool Matches(const std::wstring& text, const std::wstring& what, const std::wregex& re, bool isRegex)
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
    static void Compile(const std::wstring& text, std::wregex& re, bool& isRegex)
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
    void FillFull(int n)
    {
        SendMessageW(full, WM_SETREDRAW, FALSE, 0);
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
            const std::wstring title = header + L"   (" + std::to_wstring(keep.size()) + L")";
            LVGROUP g = {};
            g.cbSize = sizeof(g);
            g.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
            g.pszHeader = const_cast<wchar_t*>(title.c_str());
            g.iGroupId = ++groupId;
            g.stateMask = LVGS_COLLAPSIBLE;
            g.state = LVGS_COLLAPSIBLE;
            ListView_InsertGroup(full, -1, &g);
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
    std::vector<Row> Varying(int n)
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

    // ----- find in the tree

    std::vector<int> Order() const
    {
        std::vector<int> order;
        std::function<void(int)> walk = [&](int i) { for (int c : nodes[i].shown) { order.push_back(c); walk(c); } };
        if (!nodes.empty()) walk(0);
        return order;
    }

    bool Hit(int n) const { return !findText.empty() && Matches(nodes[n].text, findText, findRegex, findIsRegex); }

    void FindChanged()
    {
        findText = TextOf(findBox);
        Compile(findText, findRegex, findIsRegex);
        InvalidateRect(tree, nullptr, TRUE);
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

    void FindNext(bool back)
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

    std::vector<std::wstring> PathsOf(int n) const
    {
        std::vector<std::wstring> paths;
        for (int f : nodes[n].files)
            if (data->files[f].frame <= 0) paths.push_back(data->files[f].path);   // (an enhanced file once)
        return paths;
    }

    void CopyPaths(int n)
    {
        std::wstring text;
        for (const auto& p : PathsOf(n)) text += p + L"\r\n";
        CopyText(wnd, text);
    }

    // The rows of a list (the selected ones, or all), a tab between the columns; the groups as titles
    void CopyRows(HWND list, bool selection)
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

    void TreeMenu(LPARAM lp)
    {
        if (!ready) return;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HTREEITEM item = nullptr;
        if (pt.x == -1 && pt.y == -1)
        {
            item = TreeView_GetSelection(tree);
            RECT r = {};
            if (item && TreeView_GetItemRect(tree, item, &r, TRUE)) { pt = { r.left, r.bottom }; ClientToScreen(tree, &pt); }
        }
        else
        {
            TVHITTESTINFO hit = {};
            hit.pt = pt;
            ScreenToClient(tree, &hit.pt);
            item = TreeView_HitTest(tree, &hit);
            if (item) TreeView_SelectItem(tree, item);
        }
        if (!item) return;
        TVITEMW tv = { TVIF_PARAM };
        tv.hItem = item;
        if (!TreeView_GetItem(tree, &tv)) return;
        const int n = static_cast<int>(tv.lParam);
        HMENU menu = CreatePopupMenu();
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
        }
    }

    // ----- the floating thumbnail of the node under the mouse

    static LRESULT CALLBACK TreeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
    {
        auto self = reinterpret_cast<DicomExplorer*>(ref);
        switch (msg)
        {
        case WM_MOUSEMOVE: self->HoverMove({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); break;
        case WM_MOUSEHOVER:
            // (it is ours: the tree, without its tooltips, falls over its own handling of it)
            self->hoverArmed = false;
            self->HoverShow({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        case WM_MOUSELEAVE: self->hoverArmed = false; self->hoverItem = nullptr; self->HidePopup(); break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MOUSEWHEEL: case WM_VSCROLL: case WM_HSCROLL: case WM_KEYDOWN: case WM_KILLFOCUS:
            self->HidePopup();
            break;
        case WM_NCDESTROY: RemoveWindowSubclass(h, TreeProc, 1); break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    int NodeAt(POINT pt, HTREEITEM* where = nullptr) const
    {
        TVHITTESTINFO hit = {};
        hit.pt = pt;
        HTREEITEM item = TreeView_HitTest(tree, &hit);
        if (!item || !(hit.flags & TVHT_ONITEM)) return -1;
        if (where) *where = item;
        TVITEMW tv = { TVIF_PARAM };
        tv.hItem = item;
        return TreeView_GetItem(tree, &tv) ? static_cast<int>(tv.lParam) : -1;
    }

    // The mouse on another node: the thumbnail goes, and comes back if it stays still there for a moment
    void HoverMove(POINT pt)
    {
        if (!ready || !hoverPreview) return;
        HTREEITEM item = nullptr;
        NodeAt(pt, &item);
        if (item != hoverItem)
        {
            hoverItem = item;
            HidePopup();
            hoverArmed = false;
        }
        if (!hoverArmed && item && (!popup || !IsWindowVisible(popup)))
        {
            TRACKMOUSEEVENT t = { sizeof(t), TME_HOVER | TME_LEAVE, tree, 450 };
            hoverArmed = TrackMouseEvent(&t) != FALSE;
        }
    }

    void HoverShow(POINT pt)
    {
        if (!ready || !hoverPreview || !popup) return;
        HTREEITEM item = nullptr;
        const int n = NodeAt(pt, &item);
        if (n <= 0 || n >= static_cast<int>(nodes.size())) return;
        popupNode = n;
        popupFrame = 0;
        hoverItem = item;
        if (!thumbs.count(n)) AskThumb(n);
        PlacePopup();
    }

    void HidePopup()
    {
        if (!popup) return;
        KillTimer(popup, TimerFrame);
        if (IsWindowVisible(popup)) ShowWindow(popup, SW_HIDE);
        popupNode = -1;
    }

    // What the thumbnail of a node shows (DCMthumbnail.m): an image, its frames; a position, its images
    // (the cine); an orientation, its positions as a mosaic (each one moving, a cine of several slices); a
    // series of several orientations, one over the other; a study or a patient, a mosaic of its series
    Stack StackOf(int n) const
    {
        const GatherNode& node = nodes[n];
        const std::vector<Gathered>& files = data->files;
        Stack s;
        if (node.level == Level::Image)
        {
            const Gathered& g = files[node.files[0]];
            s.files = { { { g.path, (std::max)(0, g.frame) } } };
            s.frames = g.frame >= 0 ? 1 : g.frames;     // (a frame of an enhanced file: that one)
            return s;
        }
        std::vector<int> positions;
        if (node.level == Level::Position) positions = { n };
        else
        {
            int o = n;
            while (nodes[o].level != Level::Orientation && !nodes[o].children.empty()) o = nodes[o].children[0];
            positions = nodes[o].children;
        }
        for (int p : positions)
        {
            std::vector<FrameRef> paths;
            for (int f : nodes[p].files) paths.push_back({ files[f].path, (std::max)(0, files[f].frame) });
            s.files.push_back(std::move(paths));
        }
        if (s.files.size() == 1 && s.files[0].size() == 1 && files[nodes[positions[0]].files[0]].frame < 0) s.frames = files[nodes[positions[0]].files[0]].frames;
        // The time from image to image: the median step of the trigger times of the first position
        std::vector<double> t;
        for (int f : nodes[positions[0]].files) if (!files[f].trigger.empty()) t.push_back(atof(files[f].trigger.c_str()));
        if (t.size() > 1)
        {
            std::vector<double> steps;
            for (size_t k = 1; k < t.size(); k++) steps.push_back(std::fabs(t[k] - t[k - 1]));
            std::sort(steps.begin(), steps.end());
            s.delay = steps[steps.size() / 2];
        }
        return s;
    }

    // The orientation with most positions of a series (the one that shows it best)
    int MainOrientation(int series) const
    {
        int best = nodes[series].children[0];
        for (int o : nodes[series].children) if (nodes[o].children.size() > nodes[best].children.size()) best = o;
        return best;
    }

    void AskThumb(int n, bool big = false)
    {
        ThumbAsk a;
        a.node = n;
        a.generation = generation;
        a.big = big;
        const COLORREF bg = GetSysColor(big ? COLOR_WINDOW : COLOR_INFOBK);     // (behind a cascade: the panel's, or the popup's)
        a.background = (static_cast<DWORD>(GetRValue(bg)) << 16) | (GetGValue(bg) << 8) | GetBValue(bg);
        const GatherNode& node = nodes[n];
        if (node.level == Level::Series && node.children.size() > 1)
        {
            a.kind = ThumbAsk::Cascade;
            for (int o : node.children) a.stacks.push_back(StackOf(o));
        }
        else if (node.level == Level::Study || node.level == Level::Patient)
        {
            a.kind = ThumbAsk::Grid;
            std::function<void(int)> walk = [&](int k) {
                if (a.stacks.size() >= 20) return;
                if (nodes[k].level == Level::Series) { a.stacks.push_back(StackOf(MainOrientation(k))); return; }
                for (int c : nodes[k].children) walk(c);
            };
            walk(n);
        }
        else a.stacks.push_back(StackOf(n));
        if (worker) worker->Ask(std::move(a));
    }

    void Thumbnailed(ThumbDone* d)
    {
        std::unique_ptr<ThumbDone> done(d);
        if (d->generation != generation || d->node <= 0 || d->node >= static_cast<int>(nodes.size())) return;
        if (d->big)
        {
            posters[d->node] = std::make_shared<Thumb>(std::move(d->thumb));
            ShowPoster(d->node);
            return;
        }
        thumbs[d->node] = std::make_shared<Thumb>(std::move(d->thumb));
        if (popupNode == d->node) { popupFrame = 0; PlacePopup(); }
    }

    // The line of the node (and what the thumbnail shows of it) under the image
    std::wstring PopupText() const
    {
        if (popupNode <= 0) return std::wstring();
        std::wstring s = nodes[popupNode].text;
        auto it = thumbs.find(popupNode);
        if (it == thumbs.end()) s += L"\nReading...";
        else if (!it->second->error.empty()) s += L"\nNo image: " + it->second->error;
        else
        {
            if (!it->second->caption.empty()) s += L"\n" + it->second->caption;
            const GatherNode& node = nodes[popupNode];
            if (node.level == Level::Series && node.children.size() > 1) s += L"\n" + std::to_wstring(node.children.size()) + L" orientations, their middle images";
            if (node.level == Level::Study || node.level == Level::Patient)
            {
                size_t k = 0;
                std::function<void(int)> walk = [&](int c) { if (nodes[c].level == Level::Series) { k++; return; } for (int d : nodes[c].children) walk(d); };
                walk(popupNode);
                s += L"\n" + std::wstring(k > 20 ? L"20 of " : L"") + std::to_wstring(k) + L" series, their middle images";
            }
        }
        return s;
    }

    int PopupScale() const { return (std::max)(1, static_cast<int>((GetDpiForWindow(wnd) + 48) / 96)); }

    // The popup beside the node, its size that of the image and the text, inside the screen
    void PlacePopup()
    {
        if (!popup || popupNode <= 0 || !hoverItem) return;
        auto it = thumbs.find(popupNode);
        const Thumb* t = it != thumbs.end() && !it->second->frames.empty() ? it->second.get() : nullptr;
        const int k = PopupScale(), m = Px(6);
        const int iw = t ? t->frames[0].width * k : 0, ih = t ? t->frames[0].height * k : 0;
        HDC dc = GetDC(popup);
        HGDIOBJ old = SelectObject(dc, font);
        RECT text = { 0, 0, (std::max)(iw, Px(260)), 0 };
        const std::wstring s = PopupText();
        DrawTextW(dc, s.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(popup, dc);
        const int w = (std::max)(iw, static_cast<int>(text.right)) + 2 * m + 2, h = ih + (ih ? m : 0) + text.bottom + 2 * m + 2;
        RECT r = {};
        TreeView_GetItemRect(tree, hoverItem, &r, TRUE);
        POINT at = { r.right + Px(16), r.top };
        ClientToScreen(tree, &at);
        POINT cursor;
        GetCursorPos(&cursor);
        at.x = (std::max)(at.x, cursor.x + Px(24));
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& work = mi.rcWork;
        if (at.x + w > work.right) at.x = (std::max)(static_cast<LONG>(work.left), cursor.x - Px(24) - w);
        if (at.y + h > work.bottom) at.y = (std::max)(work.top, work.bottom - h);
        SetWindowPos(popup, HWND_TOPMOST, at.x, at.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(popup, nullptr, TRUE);
        KillTimer(popup, TimerFrame);
        if (t && t->frames.size() > 1) SetTimer(popup, TimerFrame, static_cast<UINT>(t->delay), nullptr);
    }

    void PaintPopup(HDC dc)
    {
        RECT c;
        GetClientRect(popup, &c);
        FillRect(dc, &c, GetSysColorBrush(COLOR_INFOBK));
        if (popupNode <= 0) return;
        const int k = PopupScale(), m = Px(6);
        int y = m;
        auto it = thumbs.find(popupNode);
        if (it != thumbs.end() && !it->second->frames.empty())
        {
            const Picture& p = it->second->frames[popupFrame % it->second->frames.size()];
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = p.width;
            bi.bmiHeader.biHeight = -p.height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, m, y, p.width * k, p.height * k, 0, 0, p.width, p.height, p.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            y += p.height * k + m;
        }
        RECT text = { m, y, c.right - m, c.bottom - m };
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_INFOTEXT));
        HGDIOBJ old = SelectObject(dc, font);
        const std::wstring s = PopupText();
        DrawTextW(dc, s.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto self = reinterpret_cast<DicomExplorer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self)
            {
                // (drawn aside first: no flicker while it moves)
                RECT c;
                GetClientRect(hwnd, &c);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                self->PaintPopup(mem);
                BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (self && wp == TimerFrame)
            {
                self->popupFrame++;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // ----- exports and the report

    // A name for a file or a folder: what Windows does not take, out; not too long
    static std::wstring Clean(std::wstring s)
    {
        for (auto& c : s) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        std::wstring out;
        for (wchar_t c : s) if (!(c == L' ' && (out.empty() || out.back() == L' '))) out += c;
        if (out.size() > 90) out.resize(90);
        while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
        return out.empty() ? L"_" : out;
    }

    // "S005 MR cine_sa": the number, modality and description of a series
    std::wstring SeriesName(int r) const
    {
        const Gathered& g = data->files[nodes[r].files[0]];
        wchar_t number[32] = L"S";
        if (!g.seriesNumber.empty()) swprintf_s(number, L"S%03ld", atol(g.seriesNumber.c_str()));
        std::wstring s = number;
        if (!g.modality.empty()) s += L" " + Widen(g.modality);
        if (!g.seriesDescription.empty()) s += L" " + Widen(g.seriesDescription, g.charset);
        return Clean(s);
    }

    int SeriesOf(int n) const
    {
        while (n > 0 && nodes[n].level != Level::Series) n = nodes[n].parent;
        return n;
    }

    // The folder of a patient, a study (or all): their line, without the counts
    std::wstring FolderName(int n) const
    {
        if (nodes[n].level == Level::All) return Clean(root.substr(root.find_last_of(L'\\') + 1));
        std::wstring s = Plain(n);
        const size_t open = s.rfind(L"   (");
        if (open != std::wstring::npos) s.resize(open);
        return Clean(s);
    }

    // The name of an orientation in its series: the series, and its plane when there are more
    std::wstring OrientationName(int o) const
    {
        const int r = nodes[o].parent;
        std::wstring s = SeriesName(r);
        if (nodes[r].children.size() > 1)
        {
            Header h;
            h.imageOrientation = data->files[nodes[o].files[0]].orientationText;
            std::wstring plane = PlaneName(h);
            const auto& c = nodes[r].children;
            s += L" - " + (plane.empty() ? L"orientation" : plane) + L" " + std::to_wstring(std::find(c.begin(), c.end(), o) - c.begin() + 1);
        }
        return s;
    }

    std::wstring PositionName(int p) const
    {
        const Gathered& g = data->files[nodes[p].files[0]];
        wchar_t z[40] = L"";
        if (g.hasZ) swprintf_s(z, L" Z%.2f", g.z);
        else swprintf_s(z, L" P%zu", static_cast<size_t>(std::find(nodes[nodes[p].parent].children.begin(), nodes[nodes[p].parent].children.end(), p) -
                                                            nodes[nodes[p].parent].children.begin()) + 1);
        return OrientationName(nodes[p].parent) + z;
    }

    std::wstring ImageName(int i) const
    {
        const Gathered& g = data->files[nodes[i].files[0]];
        std::wstring s = PositionName(nodes[i].parent);
        if (!g.instance.empty()) s += L" I" + Widen(g.instance);
        else s += L" " + Clean(g.path.substr(g.path.find_last_of(L'\\') + 1));
        return s;
    }

    // The NIfTI of an orientation, a position (a cine of one slice) or an image: where its positions are
    void NiftiUnit(ExportUnit& u, const std::vector<int>& positions) const
    {
        const std::vector<Gathered>& files = data->files;
        const Gathered& g = files[nodes[positions[0]].files[0]];
        u.kind = ExportUnit::Nifti;
        u.placed = g.oriented;
        memcpy(u.orientation, g.orientation, sizeof(u.orientation));
        for (int p : positions)
        {
            const Gathered& h = files[nodes[p].files[0]];
            u.placed = u.placed && h.placed;
            u.positions.push_back({ h.position[0], h.position[1], h.position[2] });
        }
        double v = 0;
        const std::wstring* t = AttributeOf(nodes[positions[0]].files[0], 0x00180088);       // SpacingBetweenSlices
        if (!t || !FirstNumber(Narrow(*t), v) || v <= 0) t = AttributeOf(nodes[positions[0]].files[0], 0x00180050);
        if (t && FirstNumber(Narrow(*t), v) && v > 0) u.thickness = v;
        u.timeStep = u.stack.delay;
        // The rescale of all of them the same: the stored values, and it in the header
        for (Uint32 tag : { 0x00281053u, 0x00281052u })
        {
            const std::wstring* first = AttributeOf(nodes[positions[0]].files[0], tag);
            for (int p : positions)
                for (int f : nodes[p].files)
                {
                    const std::wstring* other = AttributeOf(f, tag);
                    if ((first == nullptr) != (other == nullptr) || (first && *first != *other)) u.sameRescale = false;
                }
        }
    }

    // The files of a node to write, into a folder: a study (a patient, all) as folders of its own, its
    // series as files there. Images: an image as a PNG (its frames, a GIF); several images of a position
    // (a cine) as a GIF; an orientation of one image a position as a GIF through them, of more each of its
    // positions as their own GIF. NIfTI: an orientation (a position, an image) as a volume.
    void Plan(int n, bool nifti, const std::wstring& folder, ExportJob& j)
    {
        const GatherNode& node = nodes[n];
        const std::wstring at = folder + L"\\";
        switch (node.level)
        {
        case Level::All:
        case Level::Patient:
        case Level::Study:
            for (int c : node.children) Plan(c, nifti, nodes[c].level == Level::Series ? folder : at + FolderName(c), j);
            return;
        case Level::Series:
            for (int o : node.children) PlanOrientation(o, nifti, at + OrientationName(o), j);
            return;
        case Level::Orientation:
            PlanOrientation(n, nifti, at + OrientationName(n), j);
            return;
        case Level::Position:
            PlanPosition(n, nifti, at + PositionName(n), j);
            return;
        case Level::Image:
            PlanImage(n, nifti, at + ImageName(n), j);
            return;
        }
    }

    void PlanOrientation(int o, bool nifti, const std::wstring& base, ExportJob& j)
    {
        // (a report, a PDF, a presentation state: nothing to show)
        if (data->files[nodes[o].files[0]].rows == 0) { j.withoutImage++; return; }
        const auto& positions = nodes[o].children;
        if (positions.size() == 1) { PlanPosition(positions[0], nifti, base, j); return; }
        ExportUnit u;
        u.stack = StackOf(o);
        const size_t times = nodes[positions[0]].files.size();
        const bool even = std::all_of(positions.begin(), positions.end(), [&](int p) { return nodes[p].files.size() == times; });
        if (nifti)
        {
            if (!even)
            {
                j.notes.push_back(base.substr(base.find_last_of(L'\\') + 1) + L": not written, its positions have different numbers of images");
                return;
            }
            NiftiUnit(u, positions);
            u.out = base + L".nii";
            u.description = Narrow(base.substr(base.find_last_of(L'\\') + 1));
            j.total += static_cast<long>(nodes[o].files.size());
            j.units.push_back(std::move(u));
            return;
        }
        if (times == 1 && even)
        {
            u.kind = ExportUnit::Gif;
            u.out = base + L".gif";
            j.total += static_cast<long>(positions.size());
            j.units.push_back(std::move(u));
            return;
        }
        for (int p : positions) PlanPosition(p, false, base + PositionName(p).substr(OrientationName(o).size()), j);
    }

    void PlanPosition(int p, bool nifti, const std::wstring& base, ExportJob& j)
    {
        if (nodes[p].files.size() == 1) { PlanImage(nodes[p].children[0], nifti, base, j); return; }
        ExportUnit u;
        u.stack = StackOf(p);
        if (nifti)
        {
            NiftiUnit(u, { p });
            u.out = base + L".nii";
            u.description = Narrow(base.substr(base.find_last_of(L'\\') + 1));
        }
        else
        {
            u.kind = ExportUnit::Gif;
            u.overTime = true;
            u.out = base + L".gif";
        }
        j.total += static_cast<long>(nodes[p].files.size());
        j.units.push_back(std::move(u));
    }

    void PlanImage(int i, bool nifti, const std::wstring& base, ExportJob& j)
    {
        ExportUnit u;
        u.stack = StackOf(i);
        const int frames = u.stack.frames;
        if (nifti)
        {
            NiftiUnit(u, { nodes[i].parent });
            u.out = base + L".nii";
            u.description = Narrow(base.substr(base.find_last_of(L'\\') + 1));
        }
        else
        {
            u.kind = frames > 1 ? ExportUnit::Gif : ExportUnit::Png;
            u.overTime = true;
            u.out = base + (frames > 1 ? L".gif" : L".png");
        }
        j.total += frames;
        j.units.push_back(std::move(u));
    }

    // A folder to write into (the last one used, the first time Pictures)
    std::wstring PickFolder(const wchar_t* title, const wchar_t* setting)
    {
        IFileOpenDialog* d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return std::wstring();
        DWORD options = 0;
        d->GetOptions(&options);
        d->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(title);
        std::wstring start = ReadText(setting);
        if (start.empty() || !PathIsDirectoryW(start.c_str()))
        {
            PWSTR pictures = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) start = pictures;
            CoTaskMemFree(pictures);
        }
        IShellItem* at = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&at)))) { d->SetFolder(at); at->Release(); }
        std::wstring out;
        if (SUCCEEDED(d->Show(wnd)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(d->GetResult(&item)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
                item->Release();
            }
        }
        d->Release();
        if (!out.empty())
            RegSetKeyValueW(HKEY_CURRENT_USER, Settings, setting, REG_SZ, out.c_str(), static_cast<DWORD>((out.size() + 1) * sizeof(wchar_t)));
        return out;
    }

    void Export(int n, bool nifti)
    {
        if (!ready || job || n <= 0) return;
        std::wstring folder = PickFolder(nifti ? L"Export to NIfTI: the folder to write into" : L"Export the images: the folder to write into",
                                         L"ExportFolder");
        if (folder.empty()) return;
        while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
        auto j = std::make_unique<ExportJob>();
        // A study, a patient: a folder of its own
        const Level level = nodes[n].level;
        Plan(n, nifti, level == Level::Patient || level == Level::Study ? folder + L"\\" + FolderName(n) : folder, *j);
        if (j->units.empty())
        {
            SetStatus(L"Nothing to export" + (j->notes.empty() ? std::wstring() : L": " + j->notes[0]));
            return;
        }
        // The window dragged in the preview, when it shows a file of the node
        std::wstring file;
        if (!nifti && preview && preview->DraggedWindow(file, j->center, j->width))
            for (int f : nodes[n].files) if (data->files[f].path == file) { j->dragged = true; break; }
        StartJob(std::move(j));
    }

    void StartJob(std::unique_ptr<ExportJob> j)
    {
        job = std::move(j);
        job->window = wnd;
        job->message = WM_EXPORTED;
        job->thread = std::thread(RunExport, job.get());
        SetTimer(wnd, TimerJob, 250, nullptr);
        JobProgress();
    }

    void JobProgress()
    {
        if (!job) return;
        wchar_t text[200];
        swprintf_s(text, job->reportPath.empty() ? L"Exporting: %ld of %ld images...   (Esc: stop)" : L"The report: %ld of %ld thumbnails...   (Esc: stop)",
                   job->done.load(), job->total);
        SetStatus(text);
    }

    void StopJob()
    {
        if (!job) return;
        job->cancel = true;
        if (job->thread.joinable()) job->thread.join();
        job.reset();
    }

    // Over: what was written, shown in Explorer (the report: opened)
    void JobDone()
    {
        KillTimer(wnd, TimerJob);
        if (!job) return;
        if (job->thread.joinable()) job->thread.join();
        std::unique_ptr<ExportJob> j = std::move(job);
        std::wstring s;
        if (!j->reportPath.empty())
        {
            s = j->written.empty() ? L"The report was not written" : L"Report written: " + j->reportPath;
            if (!j->written.empty()) ShellExecuteW(wnd, L"open", j->reportPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else
        {
            s = std::to_wstring(j->written.size()) + (j->written.size() == 1 ? L" file written" : L" files written");
            if (!j->written.empty())
            {
                const std::wstring& w = j->written[0];
                s += L" in " + w.substr(0, w.find_last_of(L'\\'));
                ShowInExplorer({ w });
            }
        }
        if (j->withoutImage) s += L"   -   " + std::to_wstring(j->withoutImage) + L" without image (reports, PDFs...) left out";
        if (j->cancel) s += L"   -   stopped";
        if (!j->notes.empty())
        {
            s += L"   -   " + std::to_wstring(j->notes.size()) + (j->notes.size() == 1 ? L" note: " : L" notes, the first: ") + j->notes[0];
            std::wstring all;
            for (const auto& note : j->notes) all += note + L"\r\n";
            if (j->notes.size() > 1) CopyText(wnd, all);
        }
        SetStatus(s);
    }

    // ----- the report

    static std::wstring Html(const std::wstring& s)
    {
        std::wstring out;
        for (wchar_t c : s)
        {
            switch (c)
            {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
            }
        }
        return out;
    }

    // Where a report goes: a temporary folder of its own (%TEMP%\DicomStudyCount\reports), a name not used
    // yet; the browser saves it where it is wanted (Save as). The reports there of more than 7 days go.
    std::wstring ReportPath(int n) const
    {
        wchar_t temp[MAX_PATH + 1] = {};
        if (!GetTempPathW(MAX_PATH + 1, temp)) return std::wstring();
        const std::wstring folder = std::wstring(temp) + L"DicomStudyCount\\reports";
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((folder + L"\\*.html").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            const ULONGLONG week = 7ULL * 24 * 3600 * 10000000, t = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            do
            {
                const ULONGLONG w = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && t > w + week) DeleteFileW((folder + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        return FreePath(folder + L"\\DICOM report - " + (nodes[n].level == Level::Series ? SeriesName(n) : FolderName(n)) + L".html");
    }

    // A web page of what there is under a node (a lower one: its series): the patients, their studies and
    // a table of their series - a thumbnail, the planes and sizes, the spacings, the Z range, the files,
    // what the checks found. Never who the patient is, unless it is not hidden here.
    void Report(int n, std::wstring path = std::wstring())
    {
        if (!ready || job) return;
        if (n < 0) n = 0;
        if (nodes[n].level > Level::Series) n = SeriesOf(n);
        if (path.empty()) path = ReportPath(n);
        if (path.empty()) return;
        auto j = std::make_unique<ExportJob>();
        j->reportPath = path;
        const std::vector<Gathered>& files = data->files;
        auto first = [&](int k, Uint32 tag) {
            const std::wstring* v = AttributeOf(nodes[k].files[0], tag);
            return v ? *v : std::wstring();
        };
        auto mb = [&](int k) {
            long long b = 0;
            for (int f : nodes[k].files) b += files[f].bytes;
            wchar_t s[40];
            swprintf_s(s, L"%.1f", b / 1048576.0);
            return std::wstring(s);
        };
        // Where the files of a node are, from the folder of the report (".\..."): its folder; two or three,
        // each one; more, the folder that holds them all and how many
        auto relative = [&](const std::wstring& dir) {
            if (dir.size() >= root.size() && _wcsnicmp(dir.c_str(), root.c_str(), root.size()) == 0 &&
                (dir.size() == root.size() || dir[root.size()] == L'\\'))
                return L"." + dir.substr(root.size());
            return dir;
        };
        auto where = [&](int k) {
            std::vector<std::wstring> dirs;
            std::unordered_set<std::wstring> seen;
            for (int f : nodes[k].files)
            {
                const std::wstring& p = files[f].path;
                std::wstring d = p.substr(0, p.find_last_of(L'\\'));
                if (seen.insert(d).second) dirs.push_back(std::move(d));
            }
            if (dirs.empty()) return std::wstring();
            if (dirs.size() <= 3)
            {
                std::wstring s;
                for (const auto& d : dirs) s += (s.empty() ? L"" : L"   \u00B7   ") + relative(d);
                return s;
            }
            // (the longest start they share, cut at a backslash)
            std::wstring common = dirs[0];
            for (const auto& d : dirs)
            {
                size_t i = 0;
                while (i < common.size() && i < d.size() && towlower(common[i]) == towlower(d[i])) i++;
                common.resize(i);
            }
            const bool whole = std::all_of(dirs.begin(), dirs.end(), [&](const std::wstring& d) { return d.size() == common.size() || d[common.size()] == L'\\'; });
            if (!whole) common.resize(common.find_last_of(L'\\') == std::wstring::npos ? 0 : common.find_last_of(L'\\'));
            return (common.empty() ? std::wstring() : relative(common) + L"  ") + L"(" + std::to_wstring(dirs.size()) + L" folders)";
        };
        // The patients, studies and series under the node (or above it, for a series)
        std::vector<int> series;
        std::function<void(int)> walk = [&](int k) {
            if (nodes[k].level == Level::Series) { series.push_back(k); return; }
            for (int c : nodes[k].children) walk(c);
        };
        walk(n);
        size_t troubled = 0, studies = 0, images = 0;
        std::vector<int> patients, studyList;
        for (int r : series)
        {
            const int s = nodes[r].parent, p = nodes[s].parent;
            if (std::find(studyList.begin(), studyList.end(), s) == studyList.end()) studyList.push_back(s);
            if (std::find(patients.begin(), patients.end(), p) == patients.end()) patients.push_back(p);
            if (!nodes[r].flags.empty()) troubled++;
            images += FilesOf(r);
        }
        studies = studyList.size();
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t when[64];
        swprintf_s(when, L"%04d-%02d-%02d %02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
        const std::wstring scope = n == 0 ? root : root + L"   \u203A   " + nodes[n].text;

        std::wstring h;
        h += L"<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
             L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>DICOM report</title>\n<style>\n"
             L":root{--bg:#ffffff;--fg:#1c1c1e;--muted:#6b6b72;--line:#e2e2e7;--card:#f6f6f8;--warn:#a84f00;--warnbg:#fff4e6;--link:#0a58ca;"
             L"--patient:#c00000;--study:#007a00;--series:#0046dc}\n"
             L"@media (prefers-color-scheme:dark){:root{--bg:#141416;--fg:#ececf0;--muted:#a1a1aa;--line:#2c2c31;--card:#1d1d21;"
             L"--warn:#f5a524;--warnbg:#2b2112;--link:#7ab0ff;--patient:#ff7b7b;--study:#6ad46a;--series:#7aa7ff}}\n"
             L"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 \"Segoe UI\",system-ui,sans-serif}\n"
             L"main{max-width:1320px;margin:0 auto;padding:28px 16px 56px}h1{font-size:24px;margin:0 0 4px}\n"
             L".meta{color:var(--muted);overflow-wrap:anywhere}.totals{display:flex;flex-wrap:wrap;gap:10px;margin:18px 0 8px}\n"
             L".totals div{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 14px;min-width:110px}\n"
             L".totals b{display:block;font-size:22px;font-variant-numeric:tabular-nums}\n"
             L"h2{font-size:19px;margin:34px 0 4px;color:var(--patient)}h3{font-size:16px;margin:24px 0 2px;color:var(--study)}\n"
             L".wrap{overflow-x:auto;margin-top:8px;border:1px solid var(--line);border-radius:10px}\n"
             L".study{border-left:6px solid var(--c);padding:2px 0 6px 14px;margin-top:24px}.study h3{margin-top:0}"
             L".study .wrap{border:5px solid var(--c);outline:1px solid var(--line)}\n"
             L".sw{display:inline-block;width:14px;height:14px;border-radius:3px;background:var(--c);outline:1px solid var(--line);"
             L"vertical-align:-2px;margin-right:8px}\n"
             L".dot{display:inline-block;width:10px;height:10px;border-radius:50%;outline:1px solid var(--line);margin-right:6px}\n"
             L".where{font-family:Consolas,\"Cascadia Mono\",monospace;font-size:12px;color:var(--muted);overflow-wrap:anywhere}"
             L"details.patient>summary .where{margin:2px 0 0 18px}\n"
             L"summary{cursor:pointer}summary h2,summary h3{display:inline;margin:0}details.patient{margin-top:30px}"
             L"details.study>summary .meta{margin:2px 0 0 18px}details.study>summary .sub{margin:6px 0 0 18px}details.study:not([open]){padding-bottom:2px}\n"
             L".tools{margin-top:20px}.tools button{font:inherit;padding:4px 12px;margin-right:6px;border:1px solid var(--line);"
             L"border-radius:6px;background:var(--card);color:var(--fg);cursor:pointer}\n"
             L"table{border-collapse:collapse;width:100%;min-width:980px}th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--line);vertical-align:top}\n"
             L"tr:last-child td{border-bottom:0}th{color:var(--muted);font-weight:600;font-size:12px;background:var(--card)}\n"
             L"td.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}td.series{color:var(--series);font-weight:600}\n"
             L"img.t{display:block;width:auto;height:auto;max-width:128px;max-height:128px;border-radius:4px;background:#000}\n"
             L"tr.warn td{background:var(--warnbg)}.warn{color:var(--warn)}ul{margin:6px 0 0;padding-left:20px}li{margin:2px 0}\n"
             L"a{color:var(--link)}li.note{color:var(--muted)}.problems{background:var(--warnbg);border:1px solid var(--line);border-radius:10px;padding:12px 16px;margin-top:18px}\n"
             L".problems h2{margin:0;color:var(--warn);font-size:17px}small{color:var(--muted)}.notes{border:1px solid var(--line);border-radius:10px;padding:10px 16px;margin-top:12px;color:var(--muted)}.notes summary{cursor:pointer;font-weight:600}\n"
             L"footer{margin-top:40px;color:var(--muted);font-size:12px}\n</style>\n</head>\n<body>\n<main>\n";
        h += L"<h1>DICOM report</h1>\n<div class=\"meta\">" + Html(scope) + L"<br>" + when + L"</div>\n";
        auto total = [&](size_t v, const wchar_t* one, const wchar_t* many) {
            h += L"<div><b>" + std::to_wstring(v) + L"</b>" + (v == 1 ? one : many) + L"</div>";
        };
        h += L"<div class=\"totals\">";
        total(images, L"DICOM file", L"DICOM files");
        total(patients.size(), L"patient", L"patients");
        total(studies, L"study", L"studies");
        total(series.size(), L"series", L"series");
        if (troubled) h += L"<div class=\"warn\"><b>" + std::to_wstring(troubled) + L"</b>series with problems</div>";
        h += L"</div>\n";
        if (n == 0)
        {
            std::wstring also;
            if (data->others) also += std::to_wstring(data->others) + L" other files. ";
            if (data->directories) also += std::to_wstring(data->directories) + L" DICOMDIR. ";
            if (data->offline) also += std::to_wstring(data->offline) + L" files not downloaded (not read). ";
            if (!also.empty()) h += L"<div class=\"meta\">" + Html(also) + L"</div>\n";
        }
        // The color of a study, the one of its folders and icons: never under text, only as marks
        auto color = [&](int study) {
            const std::string& uid = files[nodes[study].files[0]].study;
            wchar_t c[16];
            swprintf_s(c, L"#%06X", Palette[uid.empty() ? NoStudyColor : ColorIndex(uid)]);
            return std::wstring(c);
        };
        auto dot = [&](int r) { return L"<span class=\"dot\" style=\"background:" + color(nodes[r].parent) + L"\"></span>"; };
        // (a PatientID of several people, too)
        std::vector<int> shared;
        for (int p : patients) if (nodes[p].sharing > 1) shared.push_back(p);
        if (troubled || !shared.empty())
        {
            h += L"<section class=\"problems\"><h2>\u26A0 Problems</h2><ul>\n";
            for (int p : shared)
                h += L"<li>" + Html(Plain(p)) + L": the same Patient ID, " + std::to_wstring(nodes[p].sharing) +
                     L" people (other names or birth dates): shown apart</li>\n";
            for (int r : series)
            {
                if (nodes[r].flags.empty()) continue;
                std::wstring f;
                for (size_t k = 0; k < nodes[r].flags.size(); k++) f += (k ? L", " : L"") + nodes[r].flags[k];
                h += L"<li>" + dot(r) + L"<a href=\"#s" + std::to_wstring(r) + L"\">" + Html(Plain(r)) + L"</a>: " + Html(f) + L"</li>\n";
            }
            h += L"</ul></section>\n";
        }
        // The notes (instance numbers missing, and how): folded, under the problems
        size_t noted = 0;
        for (int r : series) if (!nodes[r].notes.empty()) noted++;
        if (noted)
        {
            h += L"<details class=\"notes\"><summary>Notes: " + std::to_wstring(noted) + L" series" +
                 L" with instance numbers missing (not a problem by itself: the positions say whether a slice is missing)</summary><ul>\n";
            for (int r : series)
                for (const auto& note : nodes[r].notes)
                    h += L"<li>" + dot(r) + L"<a href=\"#s" + std::to_wstring(r) + L"\">" + Html(Plain(r)) + L"</a>: " + Html(note) + L"</li>\n";
            h += L"</ul></details>\n";
        }
        h += L"<div class=\"tools\"><button type=\"button\" onclick=\"fold(true)\">Expand all</button>"
             L"<button type=\"button\" onclick=\"fold(false)\">Collapse all</button></div>\n";
        for (int p : patients)
        {
            const Gathered& g = files[nodes[p].files[0]];
            std::wstring who = g.patient.empty() ? L"Patient (no ID)" : L"Patient " + Widen(g.patient, g.charset);
            if (!nodes[p].person.empty()) who += L" \x00B7 [" + nodes[p].person + L"]";
            if (!hideIdentity)
            {
                for (Uint32 tag : { 0x00100010u, 0x00100040u, 0x00100030u })
                {
                    std::wstring v = first(p, tag);
                    if (!v.empty()) who += L"   \u00B7   " + v;
                }
            }
            // A patient of one study folds as one: the patient and the study in the same title
            const bool alone = std::count_if(studyList.begin(), studyList.end(), [&](int s) { return nodes[s].parent == p; }) == 1;
            if (!alone)
                h += L"<details class=\"patient\" open><summary><h2>" + Html(who) + L"</h2><div class=\"meta where\">" + Html(where(p)) +
                     L"</div></summary>\n";
            for (int s : studyList)
            {
                if (nodes[s].parent != p) continue;
                const Gathered& sg = files[nodes[s].files[0]];
                std::wstring title = DateTimeText(sg.studyDate, sg.studyTime);
                const std::wstring description = Widen(sg.studyDescription, sg.charset);
                if (!description.empty()) title += (title.empty() ? L"" : L"   \u00B7   ") + description;
                if (title.empty()) title = L"Study";
                // (folded: its title, its color and its counts)
                h += std::wstring(alone ? L"<details class=\"patient study\"" : L"<details class=\"study\"") + L" style=\"--c:" + color(s) + L"\"" +
                     (studies > 3 ? L"" : L" open") + L"><summary>" + (alone ? L"<h2>" + Html(who) + L"</h2><div class=\"sub\">" : L"") +
                     L"<h3><span class=\"sw\"></span>" + Html(title) + L"</h3>" + (alone ? L"</div>" : L"") + L"\n<div class=\"meta\">";
                std::wstring line;
                auto add = [&](const std::wstring& name, const std::wstring& v) { if (!v.empty()) line += (line.empty() ? L"" : L"   \u00B7   ") + name + v; };
                if (!sg.study.empty()) add(L"", L"[" + Hash6(sg.study) + L"]");
                add(L"Accession ", first(s, 0x00080050));
                add(L"Study ID ", first(s, 0x00200010));
                add(L"", std::to_wstring(nodes[s].children.size()) + L" series");
                add(L"", HowMany(s));
                add(L"", mb(s) + L" MB");
                line = Html(line) + L"<br><span class=\"where\">" + Html(where(s)) + L"</span>";
                h += line + L"</div></summary>\n<div class=\"wrap\"><table>\n<thead><tr><th></th><th>Series</th><th>Modality</th><th>Description</th>"
                     L"<th>Plane, columns x rows x positions (x times)</th><th>Pixel, thickness (mm)</th><th>Z (mm)</th><th class=\"n\">Files</th>"
                     L"<th class=\"n\">MB</th><th>Problems, notes</th></tr></thead>\n<tbody>\n";
                for (int r : series)
                {
                    if (nodes[r].parent != s) continue;
                    const Gathered& rg = files[nodes[r].files[0]];
                    const size_t thumb = j->stacks.size();
                    j->stacks.push_back(StackOf(MainOrientation(r)));
                    std::wstring planes, spacing, zs;
                    for (int o : nodes[r].children)
                    {
                        const std::wstring br = planes.empty() ? L"" : L"<br>";
                        planes += br + Html(Size(o));
                        std::wstring sp = first(o, 0x00280030);
                        for (auto& c : sp) if (c == L'\\') c = L'x';
                        const std::wstring th = first(o, 0x00180050);
                        spacing += br + Html(sp + (th.empty() ? L"" : L",  " + th));
                        std::vector<double> z;
                        for (int c : nodes[o].children) if (files[nodes[c].files[0]].hasZ) z.push_back(files[nodes[c].files[0]].z);
                        wchar_t b[120] = L"";
                        if (z.size() == nodes[o].children.size() && z.size() > 1)
                        {
                            std::vector<double> steps;
                            for (size_t k = 1; k < z.size(); k++) steps.push_back(z[k] - z[k - 1]);
                            std::sort(steps.begin(), steps.end());
                            swprintf_s(b, L"%.1f to %.1f, step %.3g", z.front(), z.back(), steps[steps.size() / 2]);
                        }
                        else if (z.size() == 1) swprintf_s(b, L"%.1f", z[0]);
                        zs += br + b;
                    }
                    std::wstring problems;
                    for (const auto& pr : ProblemsUnder(r, 12)) problems += L"<li>" + Html(pr) + L"</li>";
                    for (const auto& note : nodes[r].notes) problems += L"<li class=\"note\">" + Html(note) + L"</li>";
                    h += L"<tr id=\"s" + std::to_wstring(r) + L"\"" + (nodes[r].flags.empty() ? L"" : L" class=\"warn\"") + L">";
                    h += L"<td><img class=\"t\" alt=\"\" src=\"{{thumb " + std::to_wstring(thumb) + L"}}\"></td>";
                    h += L"<td class=\"series\">" + Html(Widen(rg.seriesNumber)) + L"</td><td>" + Html(Widen(rg.modality)) + L"</td><td>" +
                         Html(Widen(rg.seriesDescription, rg.charset)) + L"<br><small>" + Html(DateTimeText(rg.seriesDate, rg.seriesTime)) + L"</small><br><small class=\"where\">" + Html(where(r)) + L"</small></td>";
                    h += L"<td>" + planes + L"</td><td>" + spacing + L"</td><td>" + zs + L"</td>";
                    h += L"<td class=\"n\">" + std::to_wstring(FilesOf(r)) + L"</td><td class=\"n\">" + mb(r) + L"</td>";
                    h += L"<td class=\"warn\">" + (problems.empty() ? L"" : L"<ul>" + problems + L"</ul>") + L"</td></tr>\n";
                }
                h += L"</tbody></table></div></details>\n";
            }
            if (!alone) h += L"</details>\n";
        }
        // A link to a series opens what it is folded in
        h += L"<script>\nfunction reveal(id){var e=document.getElementById(id);for(var p=e;p;p=p.parentElement)if(p.tagName==='DETAILS')p.open=true;"
             L"if(e)e.scrollIntoView();}\ndocument.addEventListener('click',function(ev){var a=ev.target.closest('a[href^=\"#s\"]');"
             L"if(a){ev.preventDefault();reveal(a.getAttribute('href').slice(1));history.replaceState(null,'',a.getAttribute('href'));}});\n"
             L"if(location.hash)reveal(location.hash.slice(1));\n"
             L"function fold(open){document.querySelectorAll('details.patient,details.study').forEach(function(d){d.open=open;});}\n</script>\n";
        h += L"<footer>Written by the DICOM explorer of DicomStudyCount. The files were only read.</footer>\n</main>\n</body>\n</html>\n";
        j->report = std::move(h);
        j->total = static_cast<long>(j->stacks.size());
        StartJob(std::move(j));
    }

    // (not into the images: a series can have thousands)
    void ExpandAll(int n, bool open)
    {
        SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
        std::function<void(int)> walk = [&](int i) {
            if (nodes[i].shown.empty() || !nodes[i].item) return;
            if (open && std::all_of(nodes[i].shown.begin(), nodes[i].shown.end(), [&](int c) { return nodes[c].level == Level::Image; }) && i != n)
                return;
            TreeView_Expand(tree, nodes[i].item, open ? TVE_EXPAND : TVE_COLLAPSE);
            for (int c : nodes[i].shown) walk(c);
        };
        walk(n);
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        if (nodes[n].item) TreeView_EnsureVisible(tree, nodes[n].item);
    }

    // ----- drawing

    static COLORREF LevelColor(int level)
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

    LRESULT Notify(NMHDR* h)
    {
        if (h->hwndFrom == tree)
        {
            switch (h->code)
            {
            case TVN_ITEMEXPANDINGW:
            {
                auto tv = reinterpret_cast<NMTREEVIEWW*>(h);
                if (tv->action & TVE_EXPAND) Insert(static_cast<int>(tv->itemNew.lParam));
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
                        if (Hit(n)) cd->clrTextBk = RGB(255, 236, 110);
                    }
                    // (the nodes of the plane under the mouse in the navigator: a black frame around them, after)
                    const LRESULT post = std::find(lit.begin(), lit.end(), n) != lit.end() ? CDRF_NOTIFYPOSTPAINT : 0;
                    if (nodes[n].level == Level::Series || nodes[n].level == Level::Patient)
                    {
                        SelectObject(cd->nmcd.hdc, boldFont);
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
        if (h->hwndFrom == full && h->code == NM_CUSTOMDRAW)
        {
            auto cd = reinterpret_cast<NMLVCUSTOMDRAW*>(h);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                const int kind = static_cast<int>(cd->nmcd.lItemlParam);
                cd->clrText = kind == 100 ? RGB(110, 110, 110) : LevelColor(kind);
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }

    Split SplitAt(POINT pt) const
    {
        if (PtInRect(&treeSplit, pt)) return Split::Tree;
        if (PtInRect(&infoSplit, pt)) return Split::Info;
        if (PtInRect(&topSplit, pt)) return Split::Top;
        return Split::None;
    }

    void DragTo(POINT pt)
    {
        const UINT dpi = GetDpiForWindow(wnd) ? GetDpiForWindow(wnd) : 96;
        RECT treeRect = {};
        GetWindowRect(tree, &treeRect);
        MapWindowPoints(nullptr, wnd, reinterpret_cast<POINT*>(&treeRect), 2);
        if (split == Split::Tree) treeWidth = MulDiv(pt.x - treeRect.left, 96, dpi);
        else if (split == Split::Info) infoWidth = MulDiv(pt.x - treeSplit.right, 96, dpi);
        else if (split == Split::Top)
        {
            const int h = treeRect.bottom - treeRect.top - Px(6) - Px(25);
            if (h > 0) topShare = (std::max)(50, (std::min)(950, static_cast<int>(static_cast<long long>(pt.y - treeRect.top) * 1000 / h)));
        }
        Layout();
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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
            if (self && reinterpret_cast<HWND>(wp) == self->tree) { self->TreeMenu(lp); return 0; }
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
                if (s != Split::None) { SetCursor(LoadCursor(nullptr, s == Split::Top ? IDC_SIZENS : IDC_SIZEWE)); return TRUE; }
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
};

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
                if (explorer.PreTranslate(m)) continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
    if (active) DeactivateActCtx(0, cookie);
    if (SUCCEEDED(com)) CoUninitialize();
}

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
