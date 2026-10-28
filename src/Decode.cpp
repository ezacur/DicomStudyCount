// The decoding of DICOM images with DCMTK (and OpenJPEG, libjxl, deflated and float frames, Philips private CT), the overlay, the DICOMDIR
#include "Shell.h"

// ---------------------------------------------------------------------------------------------
// Preview (Explorer's preview pane, Alt+P): three tabs (the DICOM columns of Explorer, to show or hide
// them; the header with names; the header with tags) over the image, whose window drags with the
// mouse. It runs in prevhost.exe, a process of its own (a crash there never takes Explorer down), at
// medium integrity (DisableLowILProcessIsolation), to reach the Explorer window for its columns.
// DCMTK reads the file from memory, decodes the pixels (uncompressed, JPEG baseline / extended /
// lossless, JPEG-LS, RLE; JPEG 2000 with OpenJPEG, JPEG XL with libjxl) and renders them with the window of the header.

void InitDcmtk()
{
    static std::once_flag once;
    std::call_once(once, []() {
        OFLog::configure(OFLogger::OFF_LOG_LEVEL);       // nobody to read its messages here
        // JPEG baseline, extended, lossless. Lossy JPEG: the color space from the JPEG markers, not
        // from the header (some devices store YCbCr and say RGB)
        DJDecoderRegistration::registerCodecs(EDC_guessLossyOnly);
        DJLSDecoderRegistration::registerCodecs();       // JPEG-LS (with the CharLS inside DCMTK)
        DcmRLEDecoderRegistration::registerCodecs();     // RLE
        // What can be read of a file written wrong, as a viewer wants it: the transfer syntax of the
        // dataset as it is (not as its meta says), implicit elements in an explicit one, a wrong length
        // of the meta group or a wrong delimiter, and on past an error where DCMTK can
        dcmAutoDetectDatasetXfer.set(OFTrue);
        dcmAcceptUnexpectedImplicitEncoding.set(OFTrue);
        dcmIgnoreFileMetaInformationGroupLength.set(OFTrue);
        dcmReplaceWrongDelimitationItem.set(OFTrue);
        dcmIgnoreParsingErrors.set(OFTrue);
        dcmUseExplLengthPixDataForEncTS.set(OFTrue);     // (compressed pixel data of an explicit length)
    });
}

// The pixels of the frame with the current window of its imaging
void Rewindow(Rendered& r)
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
std::wstring ValueOf(DcmElement* e, const std::string& charset)
{
    const Uint32 length = e->getLength();
    const DcmEVR vr = e->ident();
    if (e->getTag() == DCM_PixelData) return L"(pixel data, " + std::to_wstring(length) + L" bytes)";
    bool binary = vr == EVR_OB || vr == EVR_OW || vr == EVR_OF || vr == EVR_OD || vr == EVR_OL || vr == EVR_OV ||
                  vr == EVR_UN || vr == EVR_ox;
    if ((binary && length > 64) || length > 4096) return L"(" + std::to_wstring(length) + L" bytes)";
    OFString v;
    if (e->getOFStringArray(v).bad()) return L"(not readable)";
    std::wstring w = Widen(std::string(v.c_str(), v.length()), charset, vr == EVR_PN ? "\\=^" : "\\");
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
void DumpHeader(DcmFileFormat& ff, std::wstring& tags, std::wstring& names)
{
    OFString charset;
    // (all its values: "\ISO 2022 IR 87" begins with an empty one)
    ff.getDataset()->findAndGetOFStringArray(DCM_SpecificCharacterSet, charset);
    const std::wstring meta = L"# File meta information\r\n", dataset = L"\r\n# Dataset\r\n";
    tags = meta;
    names = meta;
    Dump(ff.getMetaInfo(), std::string(), 0, tags, names);
    tags += dataset;
    names += dataset;
    Dump(ff.getDataset(), charset.c_str(), 0, tags, names);
}

// "20230405" and "101233.5" as "2023-04-05 10:12:33"
std::wstring DateTimeText(const std::string& date, const std::string& time)
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
    ds->findAndGetOFStringArray(DCM_SpecificCharacterSet, cs);
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
bool IsJpeg2000(E_TransferSyntax x)
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
DcmDataset* DecodeJpeg2000(DcmDataset* ds, bool multiframe, int frame)
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

    opj_image_t* image = nullptr;
    auto decode = [&]() {
        if (image) { opj_image_destroy(image); image = nullptr; }
        MemoryStream mem = { code.data(), code.size(), 0 };
        opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
        if (!stream) return false;
        opj_stream_set_read_function(stream, MemRead);
        opj_stream_set_skip_function(stream, MemSkip);
        opj_stream_set_seek_function(stream, MemSeek);
        opj_stream_set_user_data(stream, &mem, nullptr);
        opj_stream_set_user_data_length(stream, code.size());
        opj_codec_t* codec = opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
        opj_dparameters_t options;
        opj_set_default_decoder_parameters(&options);
        const bool done = codec && opj_setup_decoder(codec, &options) && opj_read_header(stream, codec, &image) &&
                          opj_decode(codec, stream, image) && opj_end_decompress(codec, stream);
        opj_stream_destroy(stream);
        if (codec) opj_destroy_codec(codec);
        return done;
    };
    bool ok = decode();
    // Something after its last tile-part other than its end (a comment, "PADDING": Kakadu passes over it,
    // OpenJPEG not): the codestream cut there, with its end marker, and again
    if (!ok && !jp2)
    {
        size_t tail = 0;
        for (size_t i = 2; i + 12 <= code.size(); i++)
            if (code[i] == 0xFF && code[i + 1] == 0x90 && code[i + 2] == 0 && code[i + 3] == 10) { tail = i; break; }
        while (tail && tail + 12 <= code.size() && code[tail] == 0xFF && code[tail + 1] == 0x90)
        {
            const Uint32 length = (static_cast<Uint32>(code[tail + 6]) << 24) | (code[tail + 7] << 16) | (code[tail + 8] << 8) | code[tail + 9];
            if (length < 12 || tail + length > code.size()) { tail = 0; break; }
            tail += length;
        }
        if (tail && tail + 2 <= code.size() && !(code[tail] == 0xFF && code[tail + 1] == 0xD9))
        {
            code.resize(tail);
            code.push_back(0xFF);
            code.push_back(0xD9);
            ok = decode();
        }
    }

    DcmDataset* out = nullptr;
    if (ok && image && image->numcomps >= 1)
    {
        const OPJ_UINT32 samples = image->numcomps >= 3 ? 3 : 1;
        const opj_image_comp_t* c = image->comps;
        const OPJ_UINT32 w = c[0].w, h = c[0].h, prec = c[0].prec, sgnd = c[0].sgnd;
        // The same precision in all; the color ones may have fewer pixels, subsampled (4:2:2, 4:2:0: their
        // dx and dy against those of the first one), each of them then over its 2 or 4
        bool same = w > 0 && h > 0 && prec >= 1 && prec <= 16;
        OPJ_UINT32 rx[3] = { 1, 1, 1 }, ry[3] = { 1, 1, 1 };
        bool subsampled = false;
        for (OPJ_UINT32 k = 0; k < samples && same; k++)
        {
            same = c[k].prec == prec && c[k].sgnd == sgnd && c[k].data && c[0].dx > 0 && c[0].dy > 0 &&
                   c[k].dx % c[0].dx == 0 && c[k].dy % c[0].dy == 0;
            if (!same) break;
            rx[k] = c[k].dx / c[0].dx;
            ry[k] = c[k].dy / c[0].dy;
            same = rx[k] >= 1 && ry[k] >= 1 && c[k].w >= (w + rx[k] - 1) / rx[k] && c[k].h >= (h + ry[k] - 1) / ry[k];
            subsampled = subsampled || rx[k] > 1 || ry[k] > 1;
        }
        if (same)
        {
            out = new DcmDataset(*ds);
            out->findAndDeleteElement(DCM_PixelData);
            const size_t n = static_cast<size_t>(w) * h;
            const bool bytes = prec <= 8 && !sgnd;
            // The values within their range: lossy ones near its ends come out beyond them (65536 is no 0)
            const OPJ_INT32 lo = sgnd ? -(1 << (prec - 1)) : 0, hi = sgnd ? (1 << (prec - 1)) - 1 : (1 << prec) - 1;
            auto value = [&](OPJ_UINT32 k, OPJ_UINT32 x, OPJ_UINT32 y) {
                const OPJ_INT32 v = c[k].data[static_cast<size_t>(y / ry[k]) * c[k].w + x / rx[k]];
                return v < lo ? lo : v > hi ? hi : v;
            };
            if (bytes)
            {
                std::vector<Uint8> v(n * samples);
                for (OPJ_UINT32 y = 0, i = 0; y < h; y++)
                    for (OPJ_UINT32 x = 0; x < w; x++, i++)
                        for (OPJ_UINT32 k = 0; k < samples; k++)
                            v[static_cast<size_t>(i) * samples + k] = static_cast<Uint8>(value(k, x, y));
                out->putAndInsertUint8Array(DCM_PixelData, v.data(), static_cast<unsigned long>(v.size()));
            }
            else
            {
                std::vector<Uint16> v(n * samples);
                for (OPJ_UINT32 y = 0, i = 0; y < h; y++)
                    for (OPJ_UINT32 x = 0; x < w; x++, i++)
                        for (OPJ_UINT32 k = 0; k < samples; k++)
                            v[static_cast<size_t>(i) * samples + k] = static_cast<Uint16>(value(k, x, y));
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
                // OpenJPEG undoes the color transform of the codestream: RGB. Subsampled, there is none (it
                // needs the three of the same size): YCbCr, as the header says (YBR_FULL_422)
                out->putAndInsertString(DCM_PhotometricInterpretation, subsampled ? "YBR_FULL" : "RGB");
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

// JPEG XL (lossless; a JPEG recompressed; lossy): DCMTK has no decoder for it, libjxl decodes it
bool IsJpegXl(E_TransferSyntax x)
{
    return x == EXS_JPEGXLLossless || x == EXS_JPEGXLJPEGRecompression || x == EXS_JPEGXL;
}

// One frame (its fragments: a codestream, or one in its container) as a copy of the dataset with it as
// native pixels: gray, or three samples (a lossy one comes out of its XYB as they went in, a recompressed
// JPEG out of its YCbCr in RGB), 8 or 16 bits. Null if it cannot (floating point samples too).
DcmDataset* DecodeJpegXl(DcmDataset* ds, bool multiframe, int frame)
{
    std::vector<Uint8> code;
    if (!FrameBytes(ds, multiframe, frame, code)) return nullptr;
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
    JxlDecoder* d = decoder.get();
    if (!d || JxlDecoderSubscribeEvents(d, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) return nullptr;
    JxlDecoderSetKeepOrientation(d, JXL_TRUE);      // (the pixels as the header describes them)
    if (JxlDecoderSetInput(d, code.data(), code.size()) != JXL_DEC_SUCCESS) return nullptr;
    JxlDecoderCloseInput(d);
    JxlBasicInfo info = {};
    JxlPixelFormat format = {};
    std::vector<Uint8> pixels;
    for (bool done = false; !done; )
    {
        switch (JxlDecoderProcessInput(d))
        {
        case JXL_DEC_BASIC_INFO:
            if (JxlDecoderGetBasicInfo(d, &info) != JXL_DEC_SUCCESS || info.exponent_bits_per_sample > 0 ||
                info.bits_per_sample < 1 || info.bits_per_sample > 16 || info.xsize < 1 || info.ysize < 1 ||
                info.xsize > 65535 || info.ysize > 65535)
                return nullptr;
            format = { info.num_color_channels >= 3 ? 3u : 1u, info.bits_per_sample <= 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16,
                       JXL_LITTLE_ENDIAN, 0 };
            break;
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
        {
            size_t size = 0;
            if (JxlDecoderImageOutBufferSize(d, &format, &size) != JXL_DEC_SUCCESS) return nullptr;
            pixels.resize(size);
            if (JxlDecoderSetImageOutBuffer(d, &format, pixels.data(), size) != JXL_DEC_SUCCESS) return nullptr;
            break;
        }
        case JXL_DEC_FULL_IMAGE: done = true; break;
        default: return nullptr;                    // an error, or the codestream ends without an image
        }
    }
    const Uint16 samples = static_cast<Uint16>(format.num_channels), bits = static_cast<Uint16>(info.bits_per_sample);
    const Uint16 allocated = format.data_type == JXL_TYPE_UINT8 ? 8 : 16;
    DcmDataset* out = NativeCopy(ds, pixels, allocated);
    out->putAndInsertUint16(DCM_SamplesPerPixel, samples);
    out->putAndInsertUint16(DCM_Rows, static_cast<Uint16>(info.ysize));
    out->putAndInsertUint16(DCM_Columns, static_cast<Uint16>(info.xsize));
    out->putAndInsertUint16(DCM_BitsAllocated, allocated);
    // The bits of the header, when they fit (a signed one keeps its sign there); else those of the codestream
    Uint16 stored = 0;
    if (ds->findAndGetUint16(DCM_BitsStored, stored).bad() || stored < 1 || stored > bits) stored = bits;
    out->putAndInsertUint16(DCM_BitsStored, stored);
    out->putAndInsertUint16(DCM_HighBit, static_cast<Uint16>(stored - 1));
    if (samples == 3)
    {
        // Its samples as they were given, but for a recompressed JPEG (libjxl turns its YCbCr into RGB):
        // YBR_FULL stays so, the rest is RGB
        OFString photometric;
        ds->findAndGetOFString(DCM_PhotometricInterpretation, photometric);
        const bool ybr = photometric.substr(0, 8) == "YBR_FULL" && ds->getOriginalXfer() != EXS_JPEGXLJPEGRecompression;
        out->putAndInsertString(DCM_PhotometricInterpretation, ybr ? "YBR_FULL" : "RGB");
        out->putAndInsertUint16(DCM_PlanarConfiguration, 0);
        out->putAndInsertUint16(DCM_PixelRepresentation, 0);
    }
    else
    {
        OFString photometric;
        out->findAndGetOFString(DCM_PhotometricInterpretation, photometric);
        if (photometric != "MONOCHROME1") out->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
        out->findAndDeleteElement(DCM_PlanarConfiguration);
    }
    return out;
}

// Elscint / Philips PMSCT_RLE1 (transfer syntax 1.3.46.670589.33.1.4.1): some Elscint CT (later Picker,
// Marconi, Philips) write the image into the private (07A1,100A), with the name of its codec in
// (07A1,1011), and no (7FE0,0010). DCMTK has no decoder for it.
static const char PmsctSyntax[] = "1.3.46.670589.33.1.4.1";

bool IsPmsct(DcmFileFormat& ff)
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
DcmDataset* DecodePmsct(DcmDataset* ds)
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

// Whether a fragment starts a codestream: JPEG and JPEG-LS (their SOI, FF D8), JPEG 2000 (SOC, FF 4F, or
// a JP2 signature box), JPEG XL (FF 0A, or its container)
static bool StartsCodestream(const Uint8* b, Uint32 n)
{
    if (n >= 2 && b[0] == 0xFF && (b[1] == 0xD8 || b[1] == 0x4F || b[1] == 0x0A)) return true;
    return n >= 8 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0x0C &&
           ((b[4] == 'j' && b[5] == 'P') || (b[4] == 'J' && b[5] == 'X' && b[6] == 'L'));
}

// Frames of JPEG or JPEG-LS data in more fragments than frames, without an offset table: DCMTK cannot tell
// where each starts (it decodes the first one only)
bool UnindexedJpegFrames(DcmDataset* ds, E_TransferSyntax xfer)
{
    DcmXfer x(xfer);
    if (x.getJPEGProcess8Bit() == 0 && xfer != EXS_JPEGLSLossless && xfer != EXS_JPEGLSLossy) return false;
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 2) return false;
    DcmElement* element = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, element).bad() || !element) return false;
    auto pixels = static_cast<DcmPixelData*>(element);
    E_TransferSyntax key;
    const DcmRepresentationParameter* param = nullptr;
    pixels->getOriginalRepresentationKey(key, param);
    DcmPixelSequence* seq = nullptr;
    DcmPixelItem* table = nullptr;
    if (pixels->getEncapsulatedRepresentation(key, param, seq).bad() || !seq || seq->getItem(table, 0).bad() || !table) return false;
    return table->getLength() == 0 && static_cast<long>(seq->card()) - 1 > frames;
}

// One of those frames: its fragments as the one fragment of a copy of the dataset with that frame only,
// which DCMTK decodes then. Null if it cannot.
DcmDataset* DecodeUnindexedJpegFrame(DcmDataset* ds, E_TransferSyntax xfer, int frame)
{
    std::vector<Uint8> code;
    if (!FrameBytes(ds, true, frame, code)) return nullptr;
    if (code.size() % 2) code.push_back(0);         // (items are even)
    std::unique_ptr<DcmDataset> out(new DcmDataset(*ds));
    out->findAndDeleteElement(DCM_PixelData);
    out->findAndDeleteElement(DCM_NumberOfFrames);
    auto sequence = new DcmPixelSequence(DCM_PixelSequenceTag);
    sequence->insert(new DcmPixelItem(DCM_PixelItemTag));                    // (an empty offset table)
    auto item = new DcmPixelItem(DCM_PixelItemTag);
    item->putUint8Array(code.data(), static_cast<unsigned long>(code.size()));
    sequence->insert(item);
    auto pixels = new DcmPixelData(DCM_PixelData);
    pixels->putOriginalRepresentation(xfer, nullptr, sequence);
    out->insert(pixels, true);
    if (out->chooseRepresentation(EXS_LittleEndianExplicit, nullptr).bad() || !out->canWriteXfer(EXS_LittleEndianExplicit)) return nullptr;
    return out.release();
}

// The fragments of one frame of encapsulated pixel data (item 0 is the offset table): all of them for
// a single frame; for more, one fragment a frame when there are as many, or else the offset table, or
// else where each codestream starts
bool FrameBytes(DcmDataset* ds, bool multiframe, int frame, std::vector<Uint8>& out)
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
    // The offset table: where each frame starts, counted from the first fragment (8 bytes of header each).
    // Without one, a frame starts at each fragment that starts a codestream.
    DcmPixelItem* table = nullptr;
    Uint8* offsets = nullptr;
    if (seq->getItem(table, 0).bad() || !table || table->getLength() < static_cast<Uint32>(4 * frames) || table->getUint8Array(offsets).bad() || !offsets)
    {
        int at = -1;
        for (unsigned long i = 1; i <= fragments; i++)
        {
            DcmPixelItem* item = nullptr;
            Uint8* bytes = nullptr;
            if (seq->getItem(item, i).bad() || !item || item->getUint8Array(bytes).bad() || !bytes) return false;
            if (at < 0 || StartsCodestream(bytes, item->getLength())) at++;
            if (at > frame) break;
            if (at == frame && !append(i)) return false;
        }
        return at >= frame && !out.empty();
    }
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
DcmDataset* NativeCopy(DcmDataset* ds, const std::vector<Uint8>& bytes, Uint16 allocated)
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

// A copy of the dataset with what DCMTK needs to render it and the file leaves out or has wrong, as
// viewers take it: SamplesPerPixel 1 when absent; a PhotometricInterpretation absent or not one of
// DICOM, MONOCHROME2 (one sample) or RGB; PlanarConfiguration 0 when absent, or with 4:2:2 (which has no
// planes); PixelRepresentation 0 when absent; compressed with BitsAllocated 12 or so, 16; BitsStored and
// HighBit within it; pixels cut short, the rest zeros (as dicomread does); color of 32 bits (DCMTK renders
// it not) as 16, its top ones; a palette DCMTK takes not (16 bits, 65536 entries) applied here, as RGB.
// Null if nothing is to change. `xfer` in: the one of the dataset; out: the one to render the copy with.
DcmDataset* RepairedImage(DcmDataset* ds, E_TransferSyntax& xfer, bool* cut)
{
    std::unique_ptr<DcmDataset> out(new DcmDataset(*ds));
    bool changed = false;
    auto set = [&](const DcmTagKey& key, Uint16 v) { out->putAndInsertUint16(key, v); changed = true; };
    const bool encapsulated = DcmXfer(xfer).usesEncapsulatedFormat();
    // Compressed pixel data of an explicit length (DCMTK keeps it as bytes, not as its fragments): its
    // items, (FFFE,E000) and a length each, as the fragments of a pixel sequence again
    DcmElement* element = nullptr;
    if (encapsulated && out->findAndGetElement(DCM_PixelData, element).good() && element)
    {
        auto pixels = static_cast<DcmPixelData*>(element);
        E_TransferSyntax key;
        const DcmRepresentationParameter* param = nullptr;
        DcmPixelSequence* seq = nullptr;
        pixels->getOriginalRepresentationKey(key, param);
        Uint8* raw = nullptr;
        const Uint32 length = element->getLength();
        if ((pixels->getEncapsulatedRepresentation(key, param, seq).bad() || !seq) && element->getUint8Array(raw).good() && raw &&
            length >= 8 && raw[0] == 0xFE && raw[1] == 0xFF && raw[2] == 0x00 && raw[3] == 0xE0)
        {
            auto sequence = new DcmPixelSequence(DCM_PixelSequenceTag);
            for (Uint32 at = 0; at + 8 <= length && raw[at] == 0xFE && raw[at + 1] == 0xFF && raw[at + 2] == 0x00 && raw[at + 3] == 0xE0;)
            {
                Uint32 n = 0;
                memcpy(&n, raw + at + 4, 4);
                if (at + 8 + n > length) break;
                auto item = new DcmPixelItem(DCM_PixelItemTag);
                if (n) item->putUint8Array(raw + at + 8, n);
                sequence->insert(item);
                at += 8 + n;
            }
            auto again = new DcmPixelData(DCM_PixelData);
            again->putOriginalRepresentation(xfer, nullptr, sequence);
            out->insert(again, true);
            changed = true;
        }
    }
    Uint16 samples = 0, planar = 0, representation = 0, allocated = 0, stored = 0, high = 0;
    if (ds->findAndGetUint16(DCM_SamplesPerPixel, samples).bad() || samples == 0) set(DCM_SamplesPerPixel, samples = 1);
    OFString photometric;
    ds->findAndGetOFString(DCM_PhotometricInterpretation, photometric);
    static const char* const known[] = { "MONOCHROME1", "MONOCHROME2", "PALETTE COLOR", "RGB", "YBR_FULL", "YBR_FULL_422",
                                         "YBR_PARTIAL_422", "YBR_PARTIAL_420", "YBR_ICT", "YBR_RCT", "ARGB", "CMYK", "HSV" };
    if (std::none_of(std::begin(known), std::end(known), [&](const char* k) { return photometric == k; }))
    {
        out->putAndInsertString(DCM_PhotometricInterpretation, samples == 1 ? "MONOCHROME2" : "RGB");
        changed = true;
    }
    if (samples > 1 && (ds->findAndGetUint16(DCM_PlanarConfiguration, planar).bad() ||
                        (planar == 1 && photometric.find("_42") != OFString_npos)))
        set(DCM_PlanarConfiguration, 0);
    if (ds->findAndGetUint16(DCM_PixelRepresentation, representation).bad()) set(DCM_PixelRepresentation, 0);
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    if (encapsulated && allocated > 8 && allocated < 16) set(DCM_BitsAllocated, allocated = 16);
    if (ds->findAndGetUint16(DCM_BitsStored, stored).bad() || stored == 0 || stored > allocated) set(DCM_BitsStored, stored = allocated);
    if (ds->findAndGetUint16(DCM_HighBit, high).bad() || high >= allocated || high + 1 < stored) set(DCM_HighBit, high = stored - 1);
    Uint16 rows = 0, columns = 0;
    Sint32 frames = 1;
    out->findAndGetUint16(DCM_Rows, rows);
    out->findAndGetUint16(DCM_Columns, columns);
    if (out->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;
    const size_t pixels = static_cast<size_t>(rows) * columns * static_cast<size_t>(frames);
    DcmElement* e = nullptr;
    // Pixels cut short: the rest zeros
    if (!encapsulated && (allocated == 8 || allocated == 16) && pixels > 0 && out->findAndGetElement(DCM_PixelData, e).good() && e)
    {
        const size_t want = pixels * samples * (allocated / 8);
        Uint8* bytes = nullptr;
        Uint16* half = nullptr;
        if (e->getLength() < want && e->getLength() > 0)
        {
            if (allocated == 16 && e->getUint16Array(half).good() && half)
            {
                std::vector<Uint16> all(want / 2, 0);
                memcpy(all.data(), half, (std::min)(static_cast<size_t>(e->getLength()), want));
                out->putAndInsertUint16Array(DCM_PixelData, all.data(), static_cast<unsigned long>(all.size()));
                changed = true;
                if (cut) *cut = true;
            }
            else if (e->getUint8Array(bytes).good() && bytes)
            {
                std::vector<Uint8> all(want, 0);
                memcpy(all.data(), bytes, (std::min)(static_cast<size_t>(e->getLength()), want));
                out->putAndInsertUint8Array(DCM_PixelData, all.data(), static_cast<unsigned long>(all.size()));
                changed = true;
                if (cut) *cut = true;
            }
        }
    }
    // A palette: each value its color, RGB of 8 bits. Its descriptor: entries (0: 65536), first value, bits.
    // Entries of 8 bits come two to a word.
    if (photometric == "PALETTE COLOR" && !encapsulated && samples == 1 && (allocated == 8 || allocated == 16) && pixels > 0 &&
        out->findAndGetElement(DCM_PixelData, e).good() && e)
    {
        const DcmTagKey descriptors[3] = { DCM_RedPaletteColorLookupTableDescriptor, DCM_GreenPaletteColorLookupTableDescriptor,
                                           DCM_BluePaletteColorLookupTableDescriptor };
        const DcmTagKey tables[3] = { DCM_RedPaletteColorLookupTableData, DCM_GreenPaletteColorLookupTableData,
                                      DCM_BluePaletteColorLookupTableData };
        const DcmTagKey segmented[3] = { DCM_SegmentedRedPaletteColorLookupTableData, DCM_SegmentedGreenPaletteColorLookupTableData,
                                         DCM_SegmentedBluePaletteColorLookupTableData };
        // A segmented table (PS3.3 C.7.9.2) as its entries: segments of n values as they are (0), n from the
        // last one to a value (1), or again n segments from an offset in the words (2)
        auto expand = [](const Uint16* w, size_t count, std::vector<Uint16>& to, size_t most) {
            std::function<bool(size_t, size_t, int)> run = [&](size_t from, size_t segments, int depth) {
                if (depth > 2) return false;
                for (size_t s = 0, i = from; (segments == 0 ? i < count : s < segments) && to.size() < most; s++)
                {
                    if (i + 2 > count) return segments == 0;
                    const Uint16 type = w[i], n = w[i + 1];
                    if (type == 0)
                    {
                        if (i + 2 + n > count) return false;
                        to.insert(to.end(), w + i + 2, w + i + 2 + n);
                        i += 2 + n;
                    }
                    else if (type == 1)
                    {
                        if (i + 3 > count || to.empty()) return false;
                        const double y0 = to.back(), y1 = w[i + 2];
                        for (Uint16 k = 1; k <= n; k++) to.push_back(static_cast<Uint16>(std::lround(y0 + (y1 - y0) * k / n)));
                        i += 3;
                    }
                    else if (type == 2)
                    {
                        if (i + 4 > count) return false;
                        const size_t offset = w[i + 2] | (static_cast<size_t>(w[i + 3]) << 16);
                        if (offset >= count || !run(offset, n, depth + 1)) return false;
                        i += 4;
                    }
                    else return false;
                }
                return true;
            };
            return run(0, 0, 0) && !to.empty();
        };
        std::vector<Uint16> lut[3];
        Uint16 first[3] = {}, bits[3] = {};
        bool ok = true;
        // (its values US or SS, as files write them)
        auto descriptor = [&](const DcmTagKey& key, unsigned long at, Uint16& v) {
            Sint16 s = 0;
            if (out->findAndGetUint16(key, v, at).good()) return true;
            if (out->findAndGetSint16(key, s, at).bad()) return false;
            v = static_cast<Uint16>(s);
            return true;
        };
        for (int k = 0; k < 3 && ok; k++)
        {
            Uint16 count = 0;
            const Uint16* data = nullptr;
            unsigned long length = 0;
            ok = descriptor(descriptors[k], 0, count) && descriptor(descriptors[k], 1, first[k]) && descriptor(descriptors[k], 2, bits[k]);
            if (!ok) break;
            const size_t entries = count ? count : 65536;
            if (out->findAndGetUint16Array(segmented[k], data, &length).good() && data && length > 0)
            {
                ok = expand(data, length, lut[k], entries);
                continue;
            }
            ok = out->findAndGetUint16Array(tables[k], data, &length).good() && data && length > 0;
            if (!ok) break;
            if (bits[k] <= 8 && length * 2 >= entries)
                for (size_t i = 0; i < entries; i++) lut[k].push_back(static_cast<Uint16>((data[i / 2] >> (8 * (i % 2))) & 0xFF));
            else lut[k].assign(data, data + (std::min)(entries, static_cast<size_t>(length)));
            ok = !lut[k].empty();
        }
        Uint8* bytes = nullptr;
        Uint16* half = nullptr;
        const bool wide = allocated == 16 && e->getUint16Array(half).good() && half;
        const bool narrow = !wide && e->getUint8Array(bytes).good() && bytes;
        const size_t have = e->getLength() / (allocated / 8);
        if (ok && (wide || narrow) && have >= pixels)
        {
            std::vector<Uint8> rgb(pixels * 3);
            for (size_t i = 0; i < pixels; i++)
            {
                const int v = wide ? half[i] : bytes[i];
                for (int k = 0; k < 3; k++)
                {
                    const int at = (std::max)(0, (std::min)(v - first[k], static_cast<int>(lut[k].size()) - 1));
                    const Uint16 c = lut[k][static_cast<size_t>(at)];
                    rgb[3 * i + k] = static_cast<Uint8>(bits[k] > 8 ? c >> (bits[k] - 8) : c);
                }
            }
            out->findAndDeleteElement(DCM_PixelData);
            out->putAndInsertUint8Array(DCM_PixelData, rgb.data(), static_cast<unsigned long>(rgb.size()));
            out->putAndInsertString(DCM_PhotometricInterpretation, "RGB");
            set(DCM_SamplesPerPixel, 3);
            set(DCM_PlanarConfiguration, 0);
            set(DCM_BitsAllocated, 8);
            set(DCM_BitsStored, 8);
            set(DCM_HighBit, 7);
            set(DCM_PixelRepresentation, 0);
            xfer = EXS_LittleEndianExplicit;
            return out.release();
        }
    }
    // Color of 32 bits: its top 16 (as DCMTK holds them: in words, the high one second, or first big endian)
    e = nullptr;
    Uint16* words = nullptr;
    if (!encapsulated && allocated == 32 && samples > 1 && out->findAndGetElement(DCM_PixelData, e).good() && e &&
        e->getUint16Array(words).good() && words)
    {
        const bool big = DcmXfer(xfer).isBigEndian();
        const size_t n = e->getLength() / 4;
        const int shift = stored > 16 ? stored - 16 : 0;
        std::vector<Uint16> top(n);
        for (size_t i = 0; i < n; i++)
        {
            const Uint32 v = big ? (static_cast<Uint32>(words[2 * i]) << 16) | words[2 * i + 1]
                                 : (static_cast<Uint32>(words[2 * i + 1]) << 16) | words[2 * i];
            top[i] = static_cast<Uint16>((std::min)(v >> shift, 0xFFFFu));
        }
        out->findAndDeleteElement(DCM_PixelData);
        out->putAndInsertUint16Array(DCM_PixelData, top.data(), static_cast<unsigned long>(n));
        set(DCM_BitsAllocated, 16);
        set(DCM_BitsStored, 16);
        set(DCM_HighBit, 15);
        xfer = EXS_LittleEndianExplicit;              // (its new pixels as this machine holds them)
    }
    return changed ? out.release() : nullptr;
}

// The image again, when DCMTK cannot render it as it is: from a copy repaired (RepairedImage), or else
// compressed, from a copy of the whole dataset decompressed (DCMTK finds its frames then, where it may
// not one by one). Null if neither; `kept` holds the dataset the image reads.
std::unique_ptr<DicomImage> RenderAgain(DcmDataset* ds, E_TransferSyntax xfer, unsigned long flags, unsigned long frame,
                                        std::unique_ptr<DcmDataset>& kept, bool* cut)
{
    E_TransferSyntax as = xfer;
    std::unique_ptr<DcmDataset> fixed(RepairedImage(ds, as, cut));
    if (fixed)
    {
        auto image = std::make_unique<DicomImage>(fixed.get(), as, flags | CIF_AcrNemaCompatibility, frame, 1);
        if (image->getStatus() == EIS_Normal)
        {
            kept = std::move(fixed);
            return image;
        }
    }
    if (DcmXfer(as).usesEncapsulatedFormat())
    {
        std::unique_ptr<DcmDataset> whole(new DcmDataset(fixed ? *fixed : *ds));
        if (whole->chooseRepresentation(EXS_LittleEndianExplicit, nullptr).good() && whole->canWriteXfer(EXS_LittleEndianExplicit))
        {
            auto image = std::make_unique<DicomImage>(whole.get(), EXS_LittleEndianExplicit, flags | CIF_AcrNemaCompatibility, frame, 1);
            if (image->getStatus() == EIS_Normal)
            {
                kept = std::move(whole);
                return image;
            }
        }
    }
    return nullptr;
}

// A file DCMTK stops reading before its pixels (written wrong in ways it cannot get past): its PixelData
// found by its tag in its bytes, from the end, and taken when its length is that of the image its
// attributes describe (the bytes there are, cut short, and zeros after them) - as the readers of the bytes
// themselves do. Little or big endian, explicit or implicit VR. False if there is none such.
bool RecoverPixelData(DcmDataset* ds, const char* data, size_t size, bool* cut)
{
    Uint16 rows = 0, columns = 0, samples = 1, allocated = 0;
    Sint32 frames = 1;
    // One there, native, whose value cannot be read (the file ends before it does): found again, the bytes
    // it has and zeros after them
    DcmElement* there = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, there).good() && there)
    {
        Uint8* bytes = nullptr;
        Uint16* words = nullptr;
        // (read as its VR says: as bytes, an OW one cut short reads, but not as words)
        const bool readable = there->getVR() == EVR_OB ? there->getUint8Array(bytes).good() && bytes
                                                       : there->getUint16Array(words).good() && words;
        if (DcmXfer(ds->getOriginalXfer()).usesEncapsulatedFormat() || readable) return false;
        ds->findAndDeleteElement(DCM_PixelData);
    }
    if (ds->findAndGetUint16(DCM_Rows, rows).bad() || ds->findAndGetUint16(DCM_Columns, columns).bad() ||
        ds->findAndGetUint16(DCM_BitsAllocated, allocated).bad() || (allocated != 8 && allocated != 16))
        return false;
    ds->findAndGetUint16(DCM_SamplesPerPixel, samples);
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;
    const size_t want = static_cast<size_t>(rows) * columns * (samples ? samples : 1) * static_cast<size_t>(frames) * (allocated / 8);
    if (want == 0) return false;
    auto b = reinterpret_cast<const unsigned char*>(data);
    auto u32 = [&](size_t at, bool big) {
        return big ? (static_cast<Uint32>(b[at]) << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3]
                   : (static_cast<Uint32>(b[at + 3]) << 24) | (b[at + 2] << 16) | (b[at + 1] << 8) | b[at];
    };
    for (size_t i = size - 8; i-- > 0;)
    {
        const bool little = b[i] == 0xE0 && b[i + 1] == 0x7F && b[i + 2] == 0x10 && b[i + 3] == 0x00;
        const bool big = b[i] == 0x7F && b[i + 1] == 0xE0 && b[i + 2] == 0x00 && b[i + 3] == 0x10;
        if (!little && !big) continue;
        const bool explicitVR = i + 12 <= size && b[i + 4] == 'O' && (b[i + 5] == 'W' || b[i + 5] == 'B');
        const size_t start = i + (explicitVR ? 12 : 8);
        if (start >= size || u32(i + (explicitVR ? 8 : 4), big) != want) continue;
        // (cut short: the bytes there are, and zeros)
        std::vector<Uint8> raw(want, 0);
        memcpy(raw.data(), b + start, (std::min)(want, size - start));
        if (cut && size - start < want) *cut = true;
        if (allocated == 8) ds->putAndInsertUint8Array(DCM_PixelData, raw.data(), static_cast<unsigned long>(want));
        else
        {
            std::vector<Uint16> words(want / 2);
            for (size_t k = 0; k < words.size(); k++)
                words[k] = big ? static_cast<Uint16>((raw[2 * k] << 8) | raw[2 * k + 1]) : static_cast<Uint16>((raw[2 * k + 1] << 8) | raw[2 * k]);
            ds->putAndInsertUint16Array(DCM_PixelData, words.data(), static_cast<unsigned long>(words.size()));
        }
        return true;
    }
    return false;
}

// The last fragment of its pixels, when they are compressed (not the table of offsets): null if none
static DcmPixelItem* LastFragment(DcmDataset* ds)
{
    DcmElement* element = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, element).bad() || !element) return nullptr;
    auto pixels = static_cast<DcmPixelData*>(element);
    E_TransferSyntax key;
    const DcmRepresentationParameter* param = nullptr;
    pixels->getOriginalRepresentationKey(key, param);
    DcmPixelSequence* seq = nullptr;
    DcmPixelItem* last = nullptr;
    if (!DcmXfer(key).usesEncapsulatedFormat() || pixels->getEncapsulatedRepresentation(key, param, seq).bad() || !seq ||
        seq->card() < 2 || seq->getItem(last, seq->card() - 1).bad())
        return nullptr;
    return last;
}

// Compressed pixels the file ends inside of: DCMTK keeps their last fragment at the length it says, the
// rest of it whatever its memory held before (the image of another file, at times). That fragment as long
// as the bytes the file has of it. False if there is none such.
bool TrimLastFragment(DcmDataset* ds, const char* data, size_t size)
{
    DcmPixelItem* last = size < 8 ? nullptr : LastFragment(ds);
    if (!last) return false;
    const Uint32 length = last->getLength();
    auto b = reinterpret_cast<const unsigned char*>(data);
    // (it ends with the delimiter of its sequence: not cut)
    if (!memcmp(b + size - 8, "\xFE\xFF\xDD\xE0\0\0\0\0", 8)) return false;
    // (its item tag, from the end, with a length that goes past it; an odd one DCMTK has made even)
    const size_t from = size > static_cast<size_t>(length) + 8 ? size - length - 8 : 0;      // (it is no further back)
    for (size_t i = size - 7; i-- > from;)
    {
        if (b[i] != 0xFE || b[i + 1] != 0xFF || b[i + 2] != 0x00 || b[i + 3] != 0xE0) continue;
        const Uint32 said = static_cast<Uint32>(b[i + 4]) | (b[i + 5] << 8) | (b[i + 6] << 16) | (static_cast<Uint32>(b[i + 7]) << 24);
        if ((said != length && said + 1 != length) || i + 8 + static_cast<size_t>(said) <= size) continue;
        std::vector<Uint8> have(b + i + 8, b + size);
        if (have.size() & 1) have.push_back(0);
        return last->putUint8Array(have.data(), static_cast<unsigned long>(have.size())).good();
    }
    return false;
}

// 1-bit pixels, one a byte (0 or 255), as a copy of the dataset with them as 8 bits
static DcmDataset* AsBytes(DcmDataset* ds, const std::vector<Uint8>& wide)
{
    DcmDataset* out = NativeCopy(ds, wide, 8);
    out->putAndInsertUint16(DCM_BitsAllocated, 8);
    out->putAndInsertUint16(DCM_BitsStored, 8);
    out->putAndInsertUint16(DCM_HighBit, 7);
    out->putAndInsertUint16(DCM_PixelRepresentation, 0);
    return out;
}

// The same from n bits from the bit `first` (from the lowest bit of each byte, as DICOM packs them)
static DcmDataset* BitsAsBytes(DcmDataset* ds, const Uint8* bits, size_t first, size_t n)
{
    std::vector<Uint8> wide(n);
    for (size_t i = 0; i < n; i++) wide[i] = (bits[(first + i) / 8] >> ((first + i) % 8)) & 1 ? 255 : 0;
    return AsBytes(ds, wide);
}

// Encapsulated uncompressed (1.2.840.10008.1.2.1.98): each frame as it is, in fragments of its own,
// which DCMTK reads the name of but renders not. Null if it cannot.
DcmDataset* DecodeEncapsulatedFrame(DcmDataset* ds, bool multiframe, int frame)
{
    std::vector<Uint8> bytes;
    Uint16 allocated = 0;
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    if (!FrameBytes(ds, multiframe, frame, bytes) || (allocated != 8 && allocated != 16)) return nullptr;
    return NativeCopy(ds, bytes, allocated);
}

// Deflated image frames (1.2.840.10008.1.2.8.1): each frame compressed by deflate (raw, or with a zlib
// header), which DCMTK reads the name of but has no codec for. 1-bit frames (segmentations) come out as
// 8 bits, 0 or 255. Null if it cannot.
DcmDataset* DecodeDeflatedFrame(DcmDataset* ds, bool multiframe, int frame)
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
    if (allocated == 1) return BitsAsBytes(ds, plain.data(), 0, n);
    return NativeCopy(ds, plain, allocated);
}

// 1-bit pixels (segmentations), as they are or in RLE, which DCMTK has no decoder for at 1 bit: as they
// are, a frame need not start at a byte, and DCMTK reads only the frames that do. Its bits, from the
// lowest one of each byte, as 8 bits (0 or 255).
bool IsBitFrames(DcmDataset* ds, E_TransferSyntax xfer)
{
    Uint16 allocated = 0;
    ds->findAndGetUint16(DCM_BitsAllocated, allocated);
    return allocated == 1 && (!DcmXfer(xfer).isEncapsulated() || xfer == EXS_RLELossless) && ds->tagExists(DCM_PixelData);
}

// One RLE segment (PS3.5 G.3.1, PackBits): a byte n, then n + 1 bytes as they are (n < 128), or the
// next byte 257 - n times (n > 128); 128 is nothing. At most `most` bytes; a run cut short at the end
// stops it (as the byte that pads a segment to an even length, which reads as one more run)
static void UnpackBits(const Uint8* in, size_t length, std::vector<Uint8>& out, size_t most)
{
    out.clear();
    for (size_t i = 0; i < length && out.size() < most; )
    {
        const Uint8 n = in[i++];
        if (n < 128)
        {
            if (i + n + 1 > length) return;
            out.insert(out.end(), in + i, in + i + n + 1);
            i += n + 1;
        }
        else if (n > 128)
        {
            if (i >= length) return;
            out.insert(out.end(), static_cast<size_t>(257 - n), in[i++]);
        }
    }
}

// One 1-bit RLE frame: a header of 16 numbers (how many segments, where each starts) and its one segment,
// with the pixels packed in bits, or one a byte (as pydicom writes them)
static bool RleBits(DcmDataset* ds, int frame, size_t n, std::vector<Uint8>& bits)
{
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames < 1) frames = 1;
    std::vector<Uint8> rle;
    if (!FrameBytes(ds, frames > 1, frame, rle) || rle.size() < 64) return false;
    Uint32 header[16];
    memcpy(header, rle.data(), sizeof(header));
    if (header[0] < 1 || header[0] > 15 || header[1] < 64 || header[1] >= rle.size()) return false;
    const size_t end = header[0] > 1 && header[2] > header[1] && header[2] <= rle.size() ? header[2] : rle.size();
    UnpackBits(rle.data() + header[1], end - header[1], bits, n);
    return bits.size() >= (n + 7) / 8;
}

DcmDataset* DecodeBitFrame(DcmDataset* ds, int frame)
{
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    const size_t n = static_cast<size_t>(rows) * columns, first = static_cast<size_t>(frame) * n;
    if (n == 0) return nullptr;
    if (ds->getOriginalXfer() == EXS_RLELossless)
    {
        std::vector<Uint8> bits;
        if (!RleBits(ds, frame, n, bits)) return nullptr;
        if (bits.size() < n) return BitsAsBytes(ds, bits.data(), 0, n);
        for (auto& b : bits) b = b ? 255 : 0;
        return AsBytes(ds, bits);
    }
    DcmElement* e = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, e).bad() || !e) return nullptr;
    Uint8* bytes = nullptr;
    Uint16* words = nullptr;                        // (OW: the same bytes, little endian)
    if (e->getUint8Array(bytes).bad() || !bytes)
    {
        if (e->getUint16Array(words).bad() || !words) return nullptr;
        bytes = reinterpret_cast<Uint8*>(words);
    }
    const unsigned long count = e->getLength();
    if ((first + n + 7) / 8 > count) return nullptr;
    return BitsAsBytes(ds, bytes, first, n);
}

// Float pixel data (Parametric Maps: T1, T2, ADC... as decimal numbers, 7FE0,0008 or 7FE0,0009): the
// values of a frame, which DCMTK cannot render
bool FloatFrame(DcmDataset* ds, int frame, std::vector<double>& values)
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

bool HasFloatPixels(DcmDataset* ds) { return ds->tagExists(DCM_FloatPixelData) || ds->tagExists(DCM_DoubleFloatPixelData); }

// Why a file has no image to show: it has no pixels (a report, a PDF, a presentation state...), or
// they are in a transfer syntax that is not known here. Empty if it may have one.
std::wstring NoImage(DcmFileFormat& ff)
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
    ds->findAndGetOFStringArray(DCM_SpecificCharacterSet, charset);
    std::unordered_set<Uint32> seen;
    ReadRecords(records, first, charset.c_str(), roots, 0, seen);
    return roots;
}

// One frame of the file into the pixels, by a DicomImage of its own (a JPEG 2000 one decoded first by
// OpenJPEG). Its window: the one of the header, or the range of the pixels; `keep`: the one in use, as
// the frames change. An empty string if it is shown; if not, why (and no pixels).
// The overlay planes of an image into a map of its size (0 nothing, k the plane k - 1): false if it has
// none, or if reading them fails - files broken in odd ways make DCMTK read out of bounds here, and
// then the image is shown without them (no C++ objects in this function: it is guarded as Windows does)
static bool OverlayPlanes(DicomImage* image, Uint8* planes, int w, int h)
{
    bool any = false;
    __try
    {
        for (unsigned int p = 0; p < image->getOverlayCount(0) && p < 16; p++)
        {
            unsigned int left = 0, top = 0, pw = 0, ph = 0;
            EM_Overlay mode = EMO_Default;
            const Uint8* bits = static_cast<const Uint8*>(image->getOverlayData(p, left, top, pw, ph, mode, 0, 8, 0xff, 0, 0));
            if (!bits || pw > 65535 || ph > 65535) continue;
            for (unsigned int y = 0; y < ph; y++)
                for (unsigned int x = 0; x < pw; x++)
                {
                    const unsigned int px = left + x, py = top + y;
                    if (bits[static_cast<size_t>(y) * pw + x] && px < static_cast<unsigned int>(w) && py < static_cast<unsigned int>(h))
                    {
                        planes[static_cast<size_t>(py) * w + px] = static_cast<Uint8>(p + 1);
                        any = true;
                    }
                }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return any;
}

std::wstring ShowFrame(Rendered& r, int frame, bool keep)
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
    else if (UnindexedJpegFrames(ff.getDataset(), im->xfer))
    {
        decoded.reset(DecodeUnindexedJpegFrame(ff.getDataset(), im->xfer, frame));
        if (!decoded) return L"its frame could not be found among its fragments, or decoded";
    }
    else if (IsJpegXl(im->xfer))
    {
        decoded.reset(DecodeJpegXl(ff.getDataset(), r.frames > 1, frame));
        if (!decoded) return L"its JPEG XL data could not be decoded";
    }
    else if (IsVideo(im->xfer))
    {
        std::wstring why;
        if (!im->video) im->video = OpenVideo(ff.getDataset(), why);
        if (im->video) decoded.reset(DecodeVideoFrame(*im->video, ff.getDataset(), frame, why));
        if (!decoded) return why;
    }
    else if (IsPmsct(ff))
    {
        decoded.reset(DecodePmsct(ff.getDataset()));
        if (!decoded) return L"its PMSCT_RLE1 data could not be decoded";
    }
    else if (im->xfer == EXS_EncapsulatedUncompressedLittleEndianExplicit)
    {
        decoded.reset(DecodeEncapsulatedFrame(ff.getDataset(), r.frames > 1, frame));
        if (!decoded) return L"its encapsulated frames could not be read";
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
    // (an ACR-NEMA file, without the attributes DICOM asks for - PhotometricInterpretation...: again, as
    // DCMTK reads those)
    if (im->image->getStatus() == EIS_MissingAttribute)
        im->image = std::make_unique<DicomImage>(decoded ? static_cast<DcmObject*>(decoded.get()) : &ff,
                                                 decoded ? EXS_LittleEndianExplicit : im->xfer, CIF_UsePartialAccessToPixelData | CIF_AcrNemaCompatibility,
                                                 decoded ? 0 : static_cast<unsigned long>(frame), 1);
    // What the file leaves out or has wrong, as viewers take it: again (RenderAgain)
    if (im->image->getStatus() != EIS_Normal)
    {
        std::unique_ptr<DcmDataset> kept;
        auto again = RenderAgain(decoded ? decoded.get() : ff.getDataset(), decoded ? EXS_LittleEndianExplicit : im->xfer,
                                 CIF_UsePartialAccessToPixelData, decoded ? 0 : static_cast<unsigned long>(frame), kept);
        if (again)
        {
            im->image = std::move(again);
            decoded = std::move(kept);
        }
    }
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
    // The overlay planes (monochrome images only: DCMTK reads them with the pixels)
    im->planes.clear();
    if (im->mono && image.getOverlayCount(0) > 0)
    {
        im->planes.assign(static_cast<size_t>(w) * h, 0);
        if (!OverlayPlanes(&image, im->planes.data(), w, h)) im->planes.clear();
    }
    Rewindow(r);
    return std::wstring();
}

// The height of a pixel for its width, as the image is to be seen: its PixelSpacing (the rows apart, the
// columns apart; in the functional groups of an enhanced file too), or else the ImagerPixelSpacing of a
// projection, or else its PixelAspectRatio (vertical, horizontal: video, ultrasound); 1 if none says
static double PixelAspect(DcmDataset* ds)
{
    for (const DcmTagKey& key : { DCM_PixelSpacing, DCM_ImagerPixelSpacing, DCM_PixelAspectRatio })
    {
        OFString v;
        double two[2] = {};
        if (ds->findAndGetOFStringArray(key, v, true).good() && Numbers(std::string(v.c_str(), v.length()), two, 2) &&
            two[0] > 0 && two[1] > 0)
        {
            const double a = two[0] / two[1];
            return a < 1.0 / 16 || a > 16 || std::fabs(a - 1) < 1e-3 ? 1 : a;
        }
    }
    return 1;
}

Rendered RenderDicom(const std::vector<char>& data, Render what)
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
    // Read up to an error (garbage after the image, a file cut short...): what was read, when it has its
    // pixels, or they are found in its bytes (the summary says so)
    std::wstring readError;
    TrimLastFragment(ff.getDataset(), data.data(), data.size());
    if (st.bad() && (ff.getDataset()->tagExists(DCM_PixelData) || HasFloatPixels(ff.getDataset()) ||
                     RecoverPixelData(ff.getDataset(), data.data(), data.size())))
    {
        readError = L"  -  read up to an error: " + Widen(st.text());
        st = EC_Normal;
    }
    if (st.bad())
    {
        // Deflated files: this DCMTK has no zlib (as the one of I3Dfileformat)
        const char deflate[] = "1.2.840.10008.1.2.1.99";
        bool deflated = std::search(data.begin(), data.begin() + (data.size() < 4096 ? data.size() : 4096),
                                    deflate, deflate + sizeof(deflate) - 1) != data.begin() + (data.size() < 4096 ? data.size() : 4096);
        r.summary = deflated ? L"Deflated DICOM: not shown by this preview" : L"Not readable as DICOM: " + Widen(st.text());
        return r;
    }
    // (its pixels there, but cut short: the bytes there are, and zeros)
    if (readError.empty() && RecoverPixelData(ff.getDataset(), data.data(), data.size()))
        readError = L"  -  its pixels cut short, the rest zeros";
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
    r.aspect = PixelAspect(ds);

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
        // A video: how many frames, and their time, from the video when the header does not say
        if (im->video)
        {
            double time = 0;
            const int count = VideoFrameCount(*im->video, time);
            if (!ds->tagExists(DCM_NumberOfFrames) && count > 1) r.frames = frames = count;
            if (r.frameTime == 0 && time > 0) r.frameTime = time;
        }
        if (why.empty())
        {
            r.summary += L"  -  " + std::to_wstring(r.width) + L" x " + std::to_wstring(r.height);
            if (frames > 1) r.summary += L"  -  " + std::to_wstring(frames) + L" frames";
        }
        else r.summary += L"  -  image not shown: " + why;
    }
    else r.summary += L"  -  no image";
    r.summary += L"  -  compression: " + Compression(tsUid.c_str()) + readError;
    if (what == Render::Thumbnail) r.imaging.reset();       // only its pixels
    else Annotate(r, ds);
    return r;
}

// The whole file in memory, up to 1 GB: the other files of the series
bool ReadPath(const std::wstring& path, std::vector<char>& data)
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

// The same, from the end of the file (as much of it as that fragment could be) when it does not end with
// the delimiter of its sequence
bool TrimLastFragment(DcmDataset* ds, const std::wstring& path)
{
    DcmPixelItem* last = LastFragment(ds);
    if (!last) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {}, at = {};
    std::vector<char> tail(8);
    DWORD n = 0;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 8;
    auto read = [&](LONGLONG count) {
        tail.resize(static_cast<size_t>(count));
        at.QuadPart = size.QuadPart - count;
        return SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, tail.data(), static_cast<DWORD>(count), &n, nullptr) && n == count;
    };
    ok = ok && read(8) && memcmp(tail.data(), "\xFE\xFF\xDD\xE0\0\0\0\0", 8) &&
         read((std::min)(size.QuadPart, static_cast<LONGLONG>(last->getLength()) + 8));
    CloseHandle(h);
    return ok && TrimLastFragment(ds, tail.data(), tail.size());
}

// The whole file in memory, up to 1 GB
HRESULT ReadAll(IStream* stream, std::vector<char>& data)
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
