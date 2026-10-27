// The thumbnails of DICOM files in Explorer
#include "Shell.h"

ThumbnailProvider::ThumbnailProvider() { InterlockedIncrement(&g_locks); }

ThumbnailProvider::~ThumbnailProvider()
{
    if (stream) stream->Release();
    InterlockedDecrement(&g_locks);
}

// IUnknown
IFACEMETHODIMP ThumbnailProvider::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
    else if (riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::AddRef() { return InterlockedIncrement(&refs); }

IFACEMETHODIMP_(ULONG) ThumbnailProvider::Release()
{
    LONG r = InterlockedDecrement(&refs);
    if (r == 0) delete this;
    return r;
}

// IInitializeWithStream
IFACEMETHODIMP ThumbnailProvider::Initialize(IStream* s, DWORD)
{
    if (!s) return E_INVALIDARG;
    if (stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    stream = s;
    stream->AddRef();
    return S_OK;
}

// IThumbnailProvider: at most cx x cx, keeping the aspect ratio
IFACEMETHODIMP ThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
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
        // (its height as its pixels are high: their spacing)
        const double high = r.height * (r.aspect > 0 ? r.aspect : 1);
        int w = static_cast<int>(cx), h = static_cast<int>(cx);
        if (r.width >= high) h = static_cast<int>(std::lround(cx * high / r.width));
        else w = static_cast<int>(std::lround(cx * r.width / high));
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

// The provider from before for the extension of the file of the stream, given the same stream
HRESULT ThumbnailProvider::PreviousThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
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
