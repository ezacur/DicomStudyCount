// Pixels for the explorer: the slices, the floating thumbnails and the mosaics
#include "Shell.h"

// ----- Pixels for the explorer: the floating thumbnails of its tree, its exports (PNG, animated GIF,
// NIfTI) and the thumbnails of its report

// The first number of a value ("40\400": 40)
bool FirstNumber(const std::string& value, double& out)
{
    char* end = nullptr;
    out = strtod(value.c_str(), &end);
    return end != value.c_str() && std::isfinite(out);
}

// A frame of a file, decoded as the preview decodes it (JPEG 2000 by OpenJPEG). Stored: the values as
// they are in the file, without its rescale. An empty string if it is decoded; if not, why.
std::wstring DecodeSlice(const std::wstring& path, int frame, bool stored, Slice& s)
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
    else if (UnindexedJpegFrames(ds, xfer))
    {
        decoded.reset(DecodeUnindexedJpegFrame(ds, xfer, frame));
        if (!decoded) return L"its frame could not be found among its fragments, or decoded";
    }
    else if (IsJpegXl(xfer))
    {
        decoded.reset(DecodeJpegXl(ds, frames > 1, frame));
        if (!decoded) return L"its JPEG XL data could not be decoded";
    }
    else if (IsVideo(xfer))
    {
        std::wstring why;
        std::shared_ptr<VideoFrames> video = VideoOfFile(path, ds, why);
        if (video) decoded.reset(DecodeVideoFrame(*video, ds, frame, why));
        if (!decoded) return why;
    }
    else if (IsPmsct(ff))
    {
        decoded.reset(DecodePmsct(ds));
        if (!decoded) return L"its PMSCT_RLE1 data could not be decoded";
    }
    else if (xfer == EXS_EncapsulatedUncompressedLittleEndianExplicit)
    {
        decoded.reset(DecodeEncapsulatedFrame(ds, frames > 1, frame));
        if (!decoded) return L"its encapsulated frames could not be read";
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
    std::unique_ptr<DicomImage> made = std::make_unique<DicomImage>(decoded ? static_cast<DcmObject*>(decoded.get()) : &ff,
                                                                    decoded ? EXS_LittleEndianExplicit : xfer, flags,
                                                                    decoded ? 0 : static_cast<unsigned long>(frame), 1);
    if (made->getStatus() == EIS_MissingAttribute)     // (ACR-NEMA: as DCMTK reads it)
        made = std::make_unique<DicomImage>(decoded ? static_cast<DcmObject*>(decoded.get()) : &ff, decoded ? EXS_LittleEndianExplicit : xfer,
                                            flags | CIF_AcrNemaCompatibility, decoded ? 0 : static_cast<unsigned long>(frame), 1);
    DicomImage& image = *made;
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

bool IsGray(const Picture& p)
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
void Percentiles(const std::vector<const float*>& planes, int w, const RECT& box, double p, double q, double& a, double& b)
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
Picture Shrink(const Slice& s, const RECT& box, double lo, double hi, int tw, int th)
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
Picture Mosaic(const std::vector<Picture>& tiles, int slots)
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
Picture Cascade(const std::vector<Picture>& tiles, DWORD background)
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

// `count` of 0..n-1 from `first` to `last`, evenly (round(linspace), once each)
std::vector<int> Spread(int first, int last, int count)
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
Thumb StackThumb(const Stack& st, int single, int tile, bool middle, const std::atomic<bool>* cancel)
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
