// The colors of the studies, the icons of DICOM files (their series number on the color of their study), and the handlers chained before us
// (included by Shell.h, after the headers of the parts before it)
#pragma once

static const unsigned Palette[] = {                     // 0xRRGGBB
    0x000000, 0x404040, 0x808080, 0xBFBFBF, 0xFFFFFF,   // black, gray 25/50/75 %, white
    0xE6194B, 0x800000, 0xF58231, 0xFFD8B1, 0x9A6324,   // red, maroon, orange, apricot, brown
    0x808000, 0xBFEF45, 0x3CB44B, 0xAAFFC3, 0x469990,   // olive, lime, green, mint, teal
    0x42D4F4, 0x4363D8, 0x000075, 0xDCBEFF, 0x911EB4,   // cyan, blue, navy, lavender, purple
    0xF032E6, 0xFABED4                                  // magenta, pink
};

static const int PaletteSize = sizeof(Palette) / sizeof(Palette[0]);

static const int NoStudyColor = 2;                      // gray: DICOM without StudyInstanceUID

int ColorIndex(const std::string& uid);

struct Rgb { double r, g, b; };

Rgb Background(Rgb text);

void DrawBadge(HDC dc, int x0, int y0, int size, int color, const std::wstring& label);

HICON DrawIcon(int size, int color, const std::wstring& label);

static const wchar_t ChainKey[] = L"Software\\explorer4dicoms\\Chain";

static const wchar_t PropertyChainKey[] = L"SOFTWARE\\DicomStudyCount\\PropertyHandler\\Chain";

std::wstring Previous(HKEY root, const wchar_t* base, const std::wstring& path, const wchar_t* name);

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

bool LooksDicom(const unsigned char* b, size_t n);

bool StreamLooksDicom(IStream* stream);

std::wstring StreamName(IStream* stream);

class IconHandler : public IPersistFile, public IExtractIconW
{
public:
    IconHandler();
    virtual ~IconHandler();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP GetClassID(CLSID* clsid) override;
    IFACEMETHODIMP IsDirty() override;
    IFACEMETHODIMP Load(LPCOLESTR file, DWORD) override;
    IFACEMETHODIMP Save(LPCOLESTR, BOOL) override;
    IFACEMETHODIMP SaveCompleted(LPCOLESTR) override;
    IFACEMETHODIMP GetCurFile(LPOLESTR*) override;

    IFACEMETHODIMP GetIconLocation(UINT flags, LPWSTR file, UINT cch, int* index, UINT* outFlags) override;

    IFACEMETHODIMP Extract(LPCWSTR file, UINT index, HICON* largeIcon, HICON* smallIcon, UINT sizes) override;

private:
    LONG refs = 1;
    std::wstring path;
    IExtractIconW* previous = nullptr;              // the icon handler from before, for a file that is not DICOM

    static bool FixedIcon(const std::wstring& location, LPWSTR file, UINT cch, int* index);
};
