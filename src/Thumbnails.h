// The thumbnails of DICOM files in Explorer
// (included by Shell.h, after the headers of the parts before it)
#pragma once

// Thumbnails (Explorer's medium icons and larger): the first frame, with the series icon as a badge in
// its lower right corner. DICOM without an image (reports, DICOMDIR...) fails, and keeps its icon.
class ThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    ThumbnailProvider();
    virtual ~ThumbnailProvider();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP Initialize(IStream* s, DWORD) override;

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override;

private:
    LONG refs = 1;
    IStream* stream = nullptr;

    HRESULT PreviousThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha);
};
