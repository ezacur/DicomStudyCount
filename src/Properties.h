// The columns of Explorer: the DicomStudyCount.* properties of a DICOM file, read from its header
// (included by Shell.h, after the headers of the parts before it)
#pragma once

std::wstring Widen(const std::string& s, const std::string& charset = std::string());

std::wstring Hash6(const std::string& uid);

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

bool Numbers(const std::string& value, double* out, int n);

bool ZLevel(const Header& h, double& z);

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

static const PROPERTYKEY PKEY_SeriesKind = { FMTID_Dicom, 30 };

static const PROPERTYKEY PKEY_Segmentation = { FMTID_Dicom, 31 };

std::wstring ObjectType(const std::string& uid);

std::wstring Compression(const std::string& ts);

static const PROPERTYKEY PKEY_SensitiveData = { FMTID_Dicom, 29 };

std::wstring PlaneName(const Header& h);

std::wstring SeriesKind(const Header& h, const std::wstring& plane);

// COM object: the properties of a DICOM file, read from its header when it is initialized
class PropertyHandler : public IInitializeWithFile, public IPropertyStore, public IPropertyStoreCapabilities
{
public:
    PropertyHandler();
    virtual ~PropertyHandler();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP Initialize(LPCWSTR path, DWORD mode) override;

    IFACEMETHODIMP GetCount(DWORD* count) override;
    IFACEMETHODIMP GetAt(DWORD i, PROPERTYKEY* key) override;
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override;
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT value) override;
    IFACEMETHODIMP Commit() override;

    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY key) override;

private:
    LONG refs = 1;
    IPropertyStoreCache* cache = nullptr;
    IPropertyStore* previous = nullptr;             // the handler from before, for a file that is not DICOM
    std::wstring file;                              // a DICOM file: its segmentation column, when it is asked for
    bool segmentationRead = false;

    void InitializePrevious(LPCWSTR path, DWORD mode);

    void SetInt(REFPROPERTYKEY key, const std::string& text);

    void SetDouble(REFPROPERTYKEY key, double number);

    void SetTime(REFPROPERTYKEY key, const FILETIME& ft);

    void SetUInt(REFPROPERTYKEY key, unsigned number);

    void Set(REFPROPERTYKEY key, const std::wstring& text);
};
