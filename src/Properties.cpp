// The columns of Explorer: the DicomStudyCount.* properties of a DICOM file, read from its header
#include "Shell.h"

// DICOM text: UTF-8 when the SpecificCharacterSet says ISO_IR 192, otherwise read as Latin-1 (as the
// script's reader does: one byte, one character)
std::wstring Widen(const std::string& s, const std::string& charset)
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
// src\Explorer4Dicoms\FolderComments.cs
std::wstring Hash6(const std::string& uid)
{
    static const wchar_t Base62[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned long long h = 14695981039346656037ULL;
    for (unsigned char c : uid) { h ^= c; h *= 1099511628211ULL; }
    unsigned long long v = h % 56800235584ULL;              // 62^6
    wchar_t chars[7] = {};
    for (int i = 5; i >= 0; i--) { chars[i] = Base62[v % 62]; v /= 62; }
    return chars;
}

// Copies of every file found by the counts, read from %LOCALAPPDATA%\explorer4dicoms\duplicates
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
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\explorer4dicoms\\duplicates";
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

// The numbers of a DS / IS value ("1\0\0\0\1\0"); false unless there are exactly n of them, all finite
bool Numbers(const std::string& value, double* out, int n)
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
bool ZLevel(const Header& h, double& z)
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

// The most common SOP classes by name; any other one as its UID
std::wstring ObjectType(const std::string& uid)
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
std::wstring Compression(const std::string& ts)
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

std::wstring PlaneName(const Header& h)
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

// What kind of series a file is of, guessed (experimental) from its description, protocol and sequence,
// its image type and its plane: localizer, cine (short axis, 4-, 2-, 3-chamber), late enhancement, T1 / T2 /
// T2* mapping, perfusion, flow, angiography, diffusion, T2 / black blood; segmentations and structure sets;
// for CT, angiography and calcium score. Empty when nothing says.
static std::wstring SeriesKindIn(const Header& h, const std::wstring& plane, std::string t)
{
    t = " " + t + " ";
    for (char& c : t) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char& c : t) if (c == '_' || c == '-' || c == '/' || c == '(' || c == ')' || c == '.') c = ' ';
    auto has = [&](std::initializer_list<const char*> words) {
        for (const char* w : words) if (t.find(w) != std::string::npos) return true;
        return false;
    };
    std::string type = h.imageType;
    for (char& c : type) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (h.sopClass == "1.2.840.10008.5.1.4.1.1.66.4") return L"segmentation";
    if (h.sopClass == "1.2.840.10008.5.1.4.1.1.481.3") return L"RT structures";
    if (h.sopClass.rfind("1.2.840.10008.5.1.4.1.1.88.", 0) == 0) return L"report";
    if (h.sopClass.rfind("1.2.840.10008.5.1.4.1.1.7", 0) == 0) return L"screen capture";
    if (type.find("LOCALIZER") != std::string::npos ||
        has({ " survey", "scout", " loc ", "localizer", "localiser", "topogram", "scanogram", "tfl loc", "3 plane", "3plane", "tri pilot" }))
        return L"localizer";
    const std::string m = h.modality;
    if (m == "CT")
    {
        if (has({ "calcium", "cascore", "ca score", "agatston" })) return L"calcium score";
        if (has({ "angio", "cta", "coronar", "ccta" })) return L"CT angiography";
        return L"CT";
    }
    if (m == "US") return L"ultrasound";
    if (m == "XA") return L"X-ray angiography";
    if (m != "MR") return std::wstring();
    if (has({ " flow", "qflow", "q flow", "venc", " pc ", "phase contrast", "velocity", " vel " })) return L"flow";
    if (has({ "molli", "t1map", "t1 map", "t1 mapping", "look locker", "looklocker", "lookl", "smart1", "saskia" })) return L"T1 mapping";
    if (has({ "t2map", "t2 map", "t2 mapping", "t2 prep map" })) return L"T2 mapping";
    if (has({ "t2*", "t2star", "t2 star", "t2s " })) return L"T2* mapping";
    if (has({ "perf", "first pass", "firstpass" }) || (has({ " dyn" }) && has({ " gd", "gado", "contrast", " ca " }))) return L"perfusion";
    if (has({ " dce", "dynamic", " dyn ", " dyn" })) return L"dynamic contrast";
    double ti = 0;
    const bool inversion = Numbers(h.inversionTime, &ti, 1) && ti > 150 && ti < 500;
    if (has({ " lge", "psir", " mde", "delayed", " de ", "viab", "late gad", "late enh", "phase sensitive", "tfl psir" }) ||
        (inversion && has({ " ir ", "tfl", "tfe", "gre" })))
        return L"late enhancement";
    if (has({ "angio", " mra", "cemra", " ce mra", " tof ", "3d ce" })) return L"angiography";
    if (has({ "dti", "dwi", "diff", "tensor" })) return L"diffusion";
    double phases = 0;
    const bool many = Numbers(h.cardiacImages, &phases, 1) && phases > 1;
    if (has({ "cine", "btfe", "trufi", "fiesta", "ssfp", "retro", "sbtfe" }) || (many && !has({ "perf" })))
    {
        if (plane == L"sa") return L"cine short axis";
        if (plane == L"hla") return L"cine 4-chamber";
        if (plane == L"vla") return L"cine 2-chamber";
        if (plane == L"lvot") return L"cine 3-chamber";
        return L"cine";
    }
    if (has({ "stir", "haste", " tse", "black", "dark blood", " db ", "t2w", "t2 w" })) return L"T2-weighted";
    return std::wstring();
}

// (the description of the series first: the protocol and the sequence are often those of the whole study - "Prostate
// DWI" in all of its series - and only say when the description does not)
std::wstring SeriesKind(const Header& h, const std::wstring& plane)
{
    const std::wstring own = SeriesKindIn(h, plane, h.seriesDescription);
    if (!own.empty() && own != L"CT") return own;
    const std::wstring all = SeriesKindIn(h, plane, h.seriesDescription + " " + h.protocolName + " " + h.sequenceName);
    return all.empty() ? own : all;
}

PropertyHandler::PropertyHandler() { InterlockedIncrement(&g_locks); }

PropertyHandler::~PropertyHandler()
{
    if (cache) cache->Release();
    if (previous) previous->Release();
    InterlockedDecrement(&g_locks);
}

// IUnknown
IFACEMETHODIMP PropertyHandler::QueryInterface(REFIID riid, void** ppv)
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

IFACEMETHODIMP_(ULONG) PropertyHandler::AddRef() { return InterlockedIncrement(&refs); }

IFACEMETHODIMP_(ULONG) PropertyHandler::Release()
{
    LONG r = InterlockedDecrement(&refs);
    if (r == 0) delete this;
    return r;
}

// IInitializeWithFile: the path is needed (not just a stream) for the duplicates of the file
IFACEMETHODIMP PropertyHandler::Initialize(LPCWSTR path, DWORD mode)
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
        Set(PKEY_SeriesKind, SeriesKind(h, PlaneName(h)));
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
IFACEMETHODIMP PropertyHandler::GetCount(DWORD* count)
{
    if (previous) return previous->GetCount(count);
    if (!cache) { if (count) *count = 0; return S_OK; }
    return cache->GetCount(count);
}

IFACEMETHODIMP PropertyHandler::GetAt(DWORD i, PROPERTYKEY* key)
{
    if (previous) return previous->GetAt(i, key);
    return cache ? cache->GetAt(i, key) : E_UNEXPECTED;
}

IFACEMETHODIMP PropertyHandler::GetValue(REFPROPERTYKEY key, PROPVARIANT* value)
{
    if (previous) return previous->GetValue(key, value);
    if (!value) return E_POINTER;
    if (!cache) { PropVariantInit(value); return S_OK; }
    return cache->GetValue(key, value);
}

IFACEMETHODIMP PropertyHandler::SetValue(REFPROPERTYKEY key, REFPROPVARIANT value)
{
    return previous ? previous->SetValue(key, value) : STG_E_ACCESSDENIED;
}

IFACEMETHODIMP PropertyHandler::Commit() { return previous ? previous->Commit() : STG_E_ACCESSDENIED; }

// IPropertyStoreCapabilities: nothing can be edited (in a DICOM file)
IFACEMETHODIMP PropertyHandler::IsPropertyWritable(REFPROPERTYKEY key)
{
    if (!previous) return S_FALSE;
    IPropertyStoreCapabilities* c = nullptr;
    if (FAILED(previous->QueryInterface(IID_PPV_ARGS(&c)))) return S_OK;     // without it: all of them
    const HRESULT hr = c->IsPropertyWritable(key);
    c->Release();
    return hr;
}

// It may take the path, or a stream
void PropertyHandler::InitializePrevious(LPCWSTR path, DWORD mode)
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
void PropertyHandler::SetInt(REFPROPERTYKEY key, const std::string& text)
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

void PropertyHandler::SetDouble(REFPROPERTYKEY key, double number)
{
    PROPVARIANT v;
    InitPropVariantFromDouble(number, &v);
    cache->SetValueAndState(key, &v, PSC_NORMAL);
    PropVariantClear(&v);
}

void PropertyHandler::SetTime(REFPROPERTYKEY key, const FILETIME& ft)
{
    PROPVARIANT v;
    InitPropVariantFromFileTime(&ft, &v);
    cache->SetValueAndState(key, &v, PSC_NORMAL);
    PropVariantClear(&v);
}

void PropertyHandler::SetUInt(REFPROPERTYKEY key, unsigned number)
{
    PROPVARIANT v;
    InitPropVariantFromUInt32(number, &v);
    cache->SetValueAndState(key, &v, PSC_NORMAL);
    PropVariantClear(&v);
}

void PropertyHandler::Set(REFPROPERTYKEY key, const std::wstring& text)
{
    if (text.empty()) return;
    PROPVARIANT v;
    if (SUCCEEDED(InitPropVariantFromString(text.c_str(), &v)))
    {
        cache->SetValueAndState(key, &v, PSC_NORMAL);
        PropVariantClear(&v);
    }
}
