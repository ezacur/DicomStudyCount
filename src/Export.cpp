// The exports of the explorer (PNG, animated GIF, NIfTI) and the worker of its thumbnails
#include "Shell.h"

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

// What an export says of images whose pixels end in their files before the image does: written, the
// rest zeros, which are not in the files (so nobody takes them for data)
static std::wstring CutShort(long n)
{
    return std::to_wstring(n) + (n == 1 ? L" of its images is" : L" of its images are") + L" cut short in its file (zeros after its pixels)";
}

// The pictures of a unit: the window dragged in the preview, that of the header, or that of most of
// the values (0.5 % to 99.5 %) of up to 8 of its images
std::wstring WritePictures(IWICImagingFactory* factory, const ExportUnit& u, bool dragged, double center, double width,
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
        return !wrote ? L"it could not be written" : first.cutShort ? CutShort(1) : std::wstring();
    }
    double delay = u.overTime ? (st.delay > 0 ? st.delay : first.frameTime > 0 ? first.frameTime : 100) : 80;
    GifWriter gif(factory);
    HRESULT hr = gif.Open(u.out);
    long skipped = 0, cut = 0;
    for (size_t k = 0; k < order.size() && SUCCEEDED(hr); k++)
    {
        if (cancel) return L"stopped";
        Slice s;
        const Slice* use = &first;
        if (k > 0) use = DecodeSlice(order[k].first, order[k].second, false, s).empty() ? &s : nullptr;
        if (use && use->width == first.width && use->height == first.height && use->mono == first.mono)
        {
            hr = gif.Add(Shrink(*use, all, lo, hi, first.width, first.height), static_cast<int>(delay));
            if (use->cutShort) cut++;
        }
        else skipped++;
        done++;
    }
    if (SUCCEEDED(hr)) hr = gif.Close();
    if (FAILED(hr)) return L"it could not be written";
    wrote = true;
    std::wstring note = skipped ? std::to_wstring(skipped) + L" of its images could not be read (left out)" : std::wstring();
    if (cut) note += (note.empty() ? L"" : L"; ") + CutShort(cut);
    return note;
}

// A NIfTI-1 volume (.nii): columns, rows, positions and times, the values as stored with their rescale
// in the header (or as 32-bit floats, rescaled, when the files do not share it); RGB as 24 bits. Its
// place: the sform (and the qform) from ImagePositionPatient and ImageOrientationPatient, in RAS.
std::wstring WriteNifti(const ExportUnit& u, const std::atomic<bool>& cancel, std::atomic<long>& done, bool& wrote)
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
    long unread = 0, cut = 0;
    for (int t = 0; t < T && ok; t++)
        for (int z = 0; z < Z && ok; z++)
        {
            if (cancel) { fclose(f); return L"stopped"; }
            Slice s;
            const Slice* use = &first;
            if (t > 0 || z > 0) use = DecodeSlice(path(z, t).path, path(z, t).frame, true, s).empty() ? &s : nullptr;
            if (!use || use->width != first.width || use->height != first.height || use->mono != first.mono) { use = nullptr; unread++; }
            else if (use->cutShort) cut++;
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
    std::wstring note = unread ? std::to_wstring(unread) + L" of its images could not be read (zeros there)" : std::wstring();
    if (cut) note += (note.empty() ? L"" : L"; ") + CutShort(cut);
    return note;
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
std::wstring FreePath(const std::wstring& path)
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

void RunExport(ExportJob* j)
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

ThumbWorker::ThumbWorker(HWND w, UINT m) : window(w), message(m) { thread = std::thread([this] { Run(); }); }

ThumbWorker::~ThumbWorker()
{
    {
        std::lock_guard<std::mutex> hold(lock);
        quit = true;
        stale = true;
    }
    wake.notify_one();
    thread.join();
}

void ThumbWorker::Ask(ThumbAsk a)
{
    {
        std::lock_guard<std::mutex> hold(lock);
        const int k = a.big ? 0 : 1;
        if (running == k) stale = true;
        pending[k] = std::make_unique<ThumbAsk>(std::move(a));
    }
    wake.notify_one();
}

void ThumbWorker::Run()
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

Thumb ThumbWorker::Make(const ThumbAsk& a)
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
