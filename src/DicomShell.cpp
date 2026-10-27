// DicomShell (bin\DicomShell.dll, built by tools\Build.ps1): Explorer icon handler for DICOM files. Every DICOM file gets an icon with its
// SeriesNumber written in the color of its study (the same palette and hash as the folder boxes of
// explorer4dicoms.ps1), on the complementary color. Files that are not DICOM (or are a DICOMDIR)
// keep the generic file icon.
//
// Only the header is read, up to SeriesNumber (0020,0011), the same way as the script's reader:
// Explicit/Implicit VR Little Endian, Explicit VR Big Endian and "raw" files without preamble.
// Deflated files are DICOM but are not decompressed: they get a "?".
//
// It is also the property handler of DICOM files: the Explorer4Dicoms.* columns of Explorer
// (explorer4dicoms.propdesc), read from the same header, plus the copies of every file found by the
// last count ("duplicated in").
//
// And the preview (Explorer's preview pane) and the thumbnails of DICOM files, decoded with DCMTK.
//
// And the DICOM explorer: a window of its own, run by rundll32 (ExploreW), with the DICOM files of a
// folder as a tree of patients, studies, series, orientations, positions and images.
//
// Registered by explorer4dicoms.ps1 -Install for .dcm, .ima, .dicom and files without extension: the
// icons, preview and thumbnails per user (tools\Register-DicomIcons.ps1), the properties for the
// machine (tools\Register-DicomProperties.ps1).
// It runs inside Explorer: every error ends in the generic icon or no properties, never in an exception.
//
// Its parts: Shell.h (what they share), and a file for each one - HeaderReader, Icons, Properties, Decode,
// Preview, Thumbnails, Gather, Pixels, Export, Navigator, Plot, and the explorer (Explorer.h, Explorer*.cpp).
// This one: the factories of its COM classes, and its entry points.
#include "Shell.h"

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/EXPORT:ExploreW")              // rundll32 DicomShell.dll,Explore <folder>

HMODULE g_module = nullptr;

LONG g_locks = 0;            // live objects and LockServer calls

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
