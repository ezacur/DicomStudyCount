// Video (MPEG-2, MPEG-4 AVC/H.264, HEVC/H.265): its frames by the decoders of Windows (Media Foundation)
#include "Shell.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------------
// The pixel data of a video file is one stream (in one fragment or more, PS3.5 8.2.5 to 8.2.8): an
// elementary stream as the codec writes it, or one in a container (MPEG-2 program or transport stream,
// MP4). A container goes to the source reader of Media Foundation, which finds what it holds; an
// elementary stream straight to the decoder. MPEG-2 and HEVC need the extensions of the Microsoft
// Store (MPEG-2 Video Extension, HEVC Video Extensions); H.264 comes with Windows.

// The video transfer syntaxes: MPEG-2, MPEG-4 AVC/H.264, HEVC/H.265 (each also fragmentable)
bool IsVideo(E_TransferSyntax x) { return x >= EXS_MPEG2MainProfileAtMainLevel && x <= EXS_HEVCMain10ProfileLevel5_1; }

static GUID CodecOf(E_TransferSyntax x)
{
    if (x <= EXS_FragmentableMPEG2MainProfileHighLevel) return MFVideoFormat_MPEG2;
    if (x <= EXS_FragmentableMPEG4StereoHighProfileLevel4_2) return MFVideoFormat_H264;
    return MFVideoFormat_HEVC;
}

static std::wstring NoDecoder(const GUID& codec)
{
    if (codec == MFVideoFormat_MPEG2) return L"there is no MPEG-2 decoder in this Windows (MPEG-2 Video Extension, Microsoft Store)";
    if (codec == MFVideoFormat_HEVC) return L"there is no HEVC decoder in this Windows (HEVC Video Extensions, Microsoft Store)";
    return L"there is no H.264 decoder in this Windows";
}

// Media Foundation, once a process. Windows N has none without its Media Feature Pack: its DLLs are
// loaded only here (/DELAYLOAD), so the rest works without them. An MTA kept for the threads without COM
// that decode, and for the objects they make to outlive them.
static bool StartMediaFoundation()
{
    static const bool ok = [] {
        if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
            !LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            return false;
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        CoIncrementMTAUsage(&cookie);
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return ok;
}

struct VideoFrames
{
    std::mutex lock;
    GUID codec = GUID_NULL;
    Uint16 rows = 0, columns = 0;                   // of the header: at most this of each frame
    // A container: the source reader, the stream in memory it reads, the name it is taken by, and
    // whether it can go to a time
    ComPtr<IMFSourceReader> reader;
    ComPtr<IStream> memory;
    const wchar_t* container = nullptr;
    bool canSeek = false;
    // An elementary stream: the decoder, the stream in the pieces it is given, how many it has had
    ComPtr<IMFTransform> decoder;
    std::vector<Uint8> stream;
    std::vector<std::pair<size_t, size_t>> pieces;
    size_t fed = 0;
    bool drained = false;
    // The frames out: NV12 or P010 (a plane of Y, then one of U and V, half as high), their size in the
    // buffers, the part of it with the picture, and how their YUV becomes RGB
    GUID format = GUID_NULL;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    MFVideoArea area = {};
    bool hasArea = false, bt709 = false, fullRange = false;
    double rate = 0;                                // frames a second, if it says
    int count = 0;                                  // how many frames it has, if it can tell (0: not)
    LONGLONG first = -1;                            // the time of the first frame (100 ns), once seen
    int next = 0;                                   // the number of the frame the decoder gives next
    int last = -1;                                  // the last frame asked for, and its pixels (RGB)
    std::vector<Uint8> rgb;
    int w = 0, h = 0;
};

// The container of a stream, by its first bytes, as the name the source resolver takes it by; null for
// an elementary stream (or a program stream, taken apart here: see below)
static const wchar_t* ContainerOf(const std::vector<Uint8>& s)
{
    if (s.size() >= 12 && memcmp(&s[4], "ftyp", 4) == 0) return L"video.mp4";
    if (s.size() >= 3 * 188 && s[0] == 0x47 && s[188] == 0x47 && s[376] == 0x47) return L"video.ts";
    if (s.size() >= 3 * 192 && s[4] == 0x47 && s[196] == 0x47 && s[388] == 0x47) return L"video.m2ts";
    return nullptr;
}

// The video of an MPEG-2 program stream, or of an MPEG-1 system stream (both begin with a pack, 00 00 01
// BA), as its elementary stream: the payload of the PES packets of its first video stream (E0 to EF)
// one after the other. Media Foundation reads only some of these (not the MPEG-1 ones), and cannot go
// back in one in memory. False if it is not one, or has no video.
static bool ProgramStreamVideo(const std::vector<Uint8>& s, std::vector<Uint8>& out)
{
    out.clear();
    if (s.size() < 4 || s[0] != 0 || s[1] != 0 || s[2] != 1 || s[3] != 0xBA) return false;
    int video = -1;
    for (size_t i = 0; i + 6 <= s.size(); )
    {
        if (s[i] != 0 || s[i + 1] != 0 || s[i + 2] != 1) { i++; continue; }      // (back in step)
        const int code = s[i + 3];
        if (code == 0xBA)
        {
            // A pack: MPEG-2 (01 in its top bits: 14 bytes and its stuffing) or MPEG-1 (0010: 12)
            if ((s[i + 4] & 0xC0) == 0x40) i += 14 + (i + 13 < s.size() ? (s[i + 13] & 7) : 0);
            else i += 12;
            continue;
        }
        if (code == 0xB9) break;                    // the end
        if (code < 0xBB) { i++; continue; }
        const size_t body = i + 6, end = (std::min)(s.size(), body + (static_cast<size_t>(s[i + 4]) << 8 | s[i + 5]));
        if (code >= 0xE0 && code <= 0xEF && (video < 0 || video == code) && body < end)
        {
            video = code;
            size_t p = body;
            if ((s[p] & 0xC0) == 0x80) p += 3 + (p + 2 < end ? s[p + 2] : 0);    // MPEG-2: its header length
            else
            {
                // MPEG-1: stuffing, the buffer size, then PTS (0010), PTS and DTS (0011) or neither
                while (p < end && s[p] == 0xFF) p++;
                if (p < end && (s[p] & 0xC0) == 0x40) p += 2;
                if (p < end) p += (s[p] & 0xF0) == 0x20 ? 5 : (s[p] & 0xF0) == 0x30 ? 10 : 1;
            }
            if (p < end) out.insert(out.end(), s.begin() + p, s.begin() + end);
        }
        i = end > i ? end : i + 1;
    }
    return !out.empty();
}

// The format of the frames out
static void ReadFormat(VideoFrames& v, IMFMediaType* type)
{
    type->GetGUID(MF_MT_SUBTYPE, &v.format);
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &v.width, &v.height);
    UINT32 stride = 0;
    v.stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(stride)
                                                                          : static_cast<LONG>(v.width * (v.format == MFVideoFormat_P010 ? 2 : 1));
    v.hasArea = SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&v.area), sizeof(v.area), nullptr));
    UINT32 matrix = 0, range = 0;
    v.bt709 = SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix)) && matrix != MFVideoTransferMatrix_Unknown
                  ? matrix != MFVideoTransferMatrix_BT601 : v.height >= 720;
    v.fullRange = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)) && range == MFNominalRange_0_255;
    UINT32 numerator = 0, denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) && numerator && denominator)
        v.rate = static_cast<double>(numerator) / denominator;
}

// ----- A container, by the source reader

// The reader of the stream, from its start (again: to go back where it cannot seek)
static bool MakeReader(VideoFrames& v, std::wstring& why)
{
    why = L"its video could not be opened";
    v.reader.Reset();
    v.next = 0;
    // (a byte stream of its own each time: the source closes its own when it shuts down)
    const LARGE_INTEGER zero = {};
    ComPtr<IMFByteStream> bytes;
    ComPtr<IMFSourceResolver> resolver;
    MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
    ComPtr<IUnknown> object;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFAttributes> options;
    if (FAILED(v.memory->Seek(zero, STREAM_SEEK_SET, nullptr)) || FAILED(MFCreateMFByteStreamOnStream(v.memory.Get(), &bytes)) ||
        FAILED(MFCreateSourceResolver(&resolver)) ||
        FAILED(resolver->CreateObjectFromByteStream(bytes.Get(), v.container, MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ |
                                                    MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
                                                    nullptr, &type, &object)) ||
        FAILED(object.As(&source)) || FAILED(MFCreateAttributes(&options, 1)))
        return false;
    // (the advanced video processing: from P010, the 10 bits of HEVC Main 10, to NV12 too)
    options->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), options.Get(), &v.reader))) return false;
    const DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> want, got;
    if (FAILED(v.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
        FAILED(v.reader->SetStreamSelection(video, TRUE)))
    {
        why = L"its video has no video stream";
        return false;
    }
    if (FAILED(MFCreateMediaType(&want))) return false;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(v.reader->SetCurrentMediaType(video, nullptr, want.Get())) || FAILED(v.reader->GetCurrentMediaType(video, &got)))
    {
        why = NoDecoder(v.codec);
        return false;
    }
    ReadFormat(v, got.Get());
    // How many frames: its duration by its rate
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (v.rate > 0 && SUCCEEDED(v.reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8)
        v.count = static_cast<int>(std::llround(duration.uhVal.QuadPart * v.rate / 1e7));
    PropVariantClear(&duration);
    // Whether it can go to a time (MP4 can; an MPEG-2 program or transport stream in memory, not)
    PROPVARIANT can;
    PropVariantInit(&can);
    v.canSeek = SUCCEEDED(v.reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                             MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &can)) &&
                can.vt == VT_UI4 && (can.ulVal & MFMEDIASOURCE_CAN_SEEK);
    PropVariantClear(&can);
    return true;
}

static bool OpenReader(VideoFrames& v, const wchar_t* container, std::wstring& why)
{
    v.memory.Attach(SHCreateMemStream(v.stream.data(), static_cast<UINT>(v.stream.size())));    // (a copy)
    if (!v.memory)
    {
        why = L"its video could not be opened";
        return false;
    }
    v.container = container;
    v.stream = std::vector<Uint8>();
    return MakeReader(v, why);
}

static HRESULT NextFromReader(VideoFrames& v, ComPtr<IMFSample>& out, LONGLONG& time)
{
    for (;;)
    {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = v.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample);
        if (FAILED(hr)) return hr;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(v.reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) ReadFormat(v, type.Get());
        }
        if (sample) { out = sample; return S_OK; }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return MF_E_END_OF_STREAM;
    }
}

// ----- An elementary stream, by the decoder itself

// A decoder of the codec (synchronous: the software ones)
static ComPtr<IMFTransform> FindDecoder(const GUID& codec)
{
    ComPtr<IMFTransform> decoder;
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, codec };
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &in, nullptr, &found, &count)))
        return decoder;
    for (UINT32 i = 0; i < count; i++)
    {
        if (!decoder && FAILED(found[i]->ActivateObject(IID_PPV_ARGS(&decoder)))) decoder.Reset();
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return decoder;
}

// Its frames out in NV12 (8 bits) or else P010 (10 bits)
static bool ChooseOutput(VideoFrames& v)
{
    for (const GUID& format : { MFVideoFormat_NV12, MFVideoFormat_P010 })
        for (DWORD i = 0;; i++)
        {
            ComPtr<IMFMediaType> type;
            if (FAILED(v.decoder->GetOutputAvailableType(0, i, &type))) break;
            GUID subtype = GUID_NULL;
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != format) continue;
            if (FAILED(v.decoder->SetOutputType(0, type.Get(), 0))) continue;
            ReadFormat(v, type.Get());
            return true;
        }
    return false;
}

// The pieces it is given: H.264 and HEVC by their NAL units (each from its start code, 00 00 01, or
// 00 00 00 01); MPEG-2 by blocks, which its decoder takes as they come
static void Pieces(VideoFrames& v)
{
    const std::vector<Uint8>& s = v.stream;
    v.pieces.clear();
    if (v.codec == MFVideoFormat_MPEG2)
    {
        for (size_t at = 0; at < s.size(); at += 65536) v.pieces.emplace_back(at, (std::min)(s.size(), at + 65536));
        return;
    }
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 <= s.size(); i++)
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
        {
            starts.push_back(i > 0 && s[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    if (starts.empty() || starts[0] != 0) starts.insert(starts.begin(), 0);
    for (size_t k = 0; k < starts.size(); k++) v.pieces.emplace_back(starts[k], k + 1 < starts.size() ? starts[k + 1] : s.size());
}

static bool OpenDecoder(VideoFrames& v, std::wstring& why)
{
    v.decoder.Reset();
    v.fed = 0;
    v.drained = false;
    v.count = 0;
    v.decoder = FindDecoder(v.codec);
    if (!v.decoder)
    {
        why = NoDecoder(v.codec);
        return false;
    }
    why = L"its video could not be decoded";
    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, v.codec);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
    if (v.rows && v.columns) MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, v.columns, v.rows);
    if (FAILED(v.decoder->SetInputType(0, in.Get(), 0)) || !ChooseOutput(v)) return false;
    v.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    v.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    Pieces(v);
    // How many frames: MPEG-2, by its pictures (each starts with 00 00 01 00)
    if (v.codec == MFVideoFormat_MPEG2)
        for (size_t i = 0; i + 4 <= v.stream.size(); i++)
            if (v.stream[i] == 0 && v.stream[i + 1] == 0 && v.stream[i + 2] == 1 && v.stream[i + 3] == 0) v.count++;
    return true;
}

static HRESULT NextFromDecoder(VideoFrames& v, ComPtr<IMFSample>& out)
{
    for (int changes = 0;;)
    {
        MFT_OUTPUT_STREAM_INFO info = {};
        if (FAILED(v.decoder->GetOutputStreamInfo(0, &info))) return E_FAIL;
        ComPtr<IMFSample> sample;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
        {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(info.cbSize, &buffer)) || FAILED(sample->AddBuffer(buffer.Get())))
                return E_OUTOFMEMORY;
        }
        MFT_OUTPUT_DATA_BUFFER o = {};
        o.pSample = sample.Get();
        DWORD status = 0;
        HRESULT hr = v.decoder->ProcessOutput(0, 1, &o, &status);
        if (o.pEvents) o.pEvents->Release();
        if (SUCCEEDED(hr))
        {
            if (!sample) sample.Attach(o.pSample);    // (one of its own)
            out = sample;
            return S_OK;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            if (++changes > 8 || !ChooseOutput(v)) return hr;
            continue;
        }
        if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
        if (v.drained) return MF_E_END_OF_STREAM;
        if (v.fed == v.pieces.size())
        {
            // All of it given: the frames it still holds
            v.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            v.decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            v.drained = true;
            continue;
        }
        const auto piece = v.pieces[v.fed++];
        const DWORD size = static_cast<DWORD>(piece.second - piece.first);
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> input;
        BYTE* to = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(buffer->Lock(&to, nullptr, nullptr))) return E_OUTOFMEMORY;
        memcpy(to, v.stream.data() + piece.first, size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        if (FAILED(MFCreateSample(&input)) || FAILED(input->AddBuffer(buffer.Get()))) return E_OUTOFMEMORY;
        hr = v.decoder->ProcessInput(0, input.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return hr;
    }
}

// ----- The frames

// Back to the first frame; in a container that can seek, to the time of a frame instead (the reader
// goes to the key frame before it, and the frames out say which they are by their times). One that
// cannot (some say they can, and then fail: an MPEG-2 program stream in memory) starts again from the
// first frame, a reader anew, and never seeks after that.
static bool Restart(VideoFrames& v, int frame = 0)
{
    v.next = 0;
    if (v.reader && v.canSeek)
    {
        PROPVARIANT start;
        PropVariantInit(&start);
        start.vt = VT_I8;
        start.hVal.QuadPart = frame > 0 ? v.first + static_cast<LONGLONG>(frame * 1e7 / v.rate) : 0;
        if (SUCCEEDED(v.reader->SetCurrentPosition(GUID_NULL, start)))
        {
            if (frame > 0) v.next = -1;             // (not known until a frame comes out)
            return true;
        }
        v.canSeek = false;
    }
    if (v.reader)
    {
        std::wstring why;
        if (!MakeReader(v, why)) return false;
        v.canSeek = false;
        return true;
    }
    // An elementary stream: a decoder anew (one drained at the end takes nothing more, flushed or not)
    std::wstring why;
    return OpenDecoder(v, why);
}

// A frame (NV12 or P010) as RGB: the part with the picture, at most the size of the header
static bool ToRgb(VideoFrames& v, IMFSample* sample)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* data = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    const bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch));
    if (!locked2d)
    {
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
        pitch = v.stride;
        if (pitch <= 0 || static_cast<size_t>(length) < static_cast<size_t>(pitch) * v.height * 3 / 2)
        {
            buffer->Unlock();
            return false;
        }
    }
    int left = 0, top = 0, w = static_cast<int>(v.width), h = static_cast<int>(v.height);
    if (v.hasArea && v.area.Area.cx > 0 && v.area.Area.cy > 0)
    {
        left = v.area.OffsetX.value;
        top = v.area.OffsetY.value;
        w = (std::min)(static_cast<int>(v.area.Area.cx), w - left);
        h = (std::min)(static_cast<int>(v.area.Area.cy), h - top);
    }
    if (v.columns && static_cast<int>(v.columns) < w) w = v.columns;
    if (v.rows && static_cast<int>(v.rows) < h) h = v.rows;
    const bool wide = v.format == MFVideoFormat_P010;   // (16-bit samples, the 10 bits at the top)
    const BYTE* chroma = data + static_cast<size_t>(pitch) * v.height;
    auto sampleAt = [&](const BYTE* row, int x) { return wide ? row[2 * x + 1] : row[x]; };
    // BT.601 or BT.709, in their video range (16-235) or the whole one
    const double kr = v.bt709 ? 0.2126 : 0.299, kb = v.bt709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    const double ys = v.fullRange ? 1 : 255.0 / 219, cs = v.fullRange ? 1 : 255.0 / 224, y0 = v.fullRange ? 0 : 16;
    auto clamp = [](double c) { return static_cast<Uint8>(c < 0 ? 0 : c > 255 ? 255 : c + 0.5); };
    v.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h && w > 0; y++)
    {
        const BYTE* luma = data + static_cast<size_t>(pitch) * (top + y);
        const BYTE* uv = chroma + static_cast<size_t>(pitch) * ((top + y) / 2);
        Uint8* out = &v.rgb[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; x++)
        {
            const int c = (left + x) / 2 * 2;
            const double Y = (sampleAt(luma, left + x) - y0) * ys, U = (sampleAt(uv, c) - 128) * cs, V = (sampleAt(uv, c + 1) - 128) * cs;
            out[3 * x] = clamp(Y + 2 * (1 - kr) * V);
            out[3 * x + 1] = clamp(Y - 2 * (1 - kb) * kb / kg * U - 2 * (1 - kr) * kr / kg * V);
            out[3 * x + 2] = clamp(Y + 2 * (1 - kb) * U);
        }
    }
    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();
    v.w = w;
    v.h = h;
    return w > 0 && h > 0;
}

std::shared_ptr<VideoFrames> OpenVideo(DcmDataset* ds, std::wstring& why)
{
    if (!StartMediaFoundation())
    {
        why = L"this Windows has no Media Foundation (Windows N: its Media Feature Pack)";
        return nullptr;
    }
    auto v = std::make_shared<VideoFrames>();
    v->codec = CodecOf(ds->getOriginalXfer());
    ds->findAndGetUint16(DCM_Rows, v->rows);
    ds->findAndGetUint16(DCM_Columns, v->columns);
    if (!FrameBytes(ds, false, 0, v->stream))
    {
        why = L"its video could not be read";
        return nullptr;
    }
    std::vector<Uint8> video;
    if (ProgramStreamVideo(v->stream, video)) v->stream.swap(video);
    const wchar_t* container = ContainerOf(v->stream);
    if (container ? !OpenReader(*v, container, why) : !OpenDecoder(*v, why)) return nullptr;
    return v;
}

std::shared_ptr<VideoFrames> VideoOfFile(const std::wstring& path, DcmDataset* ds, std::wstring& why)
{
    // (never destroyed: not after Media Foundation, as the process ends)
    static std::mutex lock;
    static auto* last = new std::pair<std::wstring, std::shared_ptr<VideoFrames>>();
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    wchar_t stamp[64];
    swprintf_s(stamp, L"|%08lX%08lX|%08lX", a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime, a.nFileSizeLow);
    const std::wstring key = path + stamp;
    std::lock_guard<std::mutex> hold(lock);
    if (last->first != key || !last->second)
    {
        last->second = OpenVideo(ds, why);
        last->first = last->second ? key : std::wstring();
    }
    return last->second;
}

DcmDataset* DecodeVideoFrame(VideoFrames& v, DcmDataset* ds, int frame, std::wstring& why)
{
    std::lock_guard<std::mutex> hold(v.lock);
    if (frame != v.last)
    {
        v.last = -1;
        // A container with the times of its frames: far from where it is (back, or more than a second
        // ahead), to the time of the frame; else, back to the start when it is behind
        const bool timed = v.reader && v.canSeek && v.rate > 0 && v.first >= 0;
        bool sought = timed && frame > 0 && (frame < v.next || frame > v.next + v.rate);
        bool ok = true;
        if (sought)
        {
            ok = Restart(v, frame);
            sought = v.next == -1;                  // (or it could not, and goes from the start)
        }
        else if (frame < v.next) ok = Restart(v);
        if (!ok)
        {
            why = L"its video could not go back to its start";
            return nullptr;
        }
        // The frames before it, decoded and left
        for (;;)
        {
            ComPtr<IMFSample> sample;
            LONGLONG time = 0;
            const HRESULT hr = v.reader ? NextFromReader(v, sample, time) : NextFromDecoder(v, sample);
            if (hr == MF_E_END_OF_STREAM)
            {
                why = sought ? L"its video could not go to frame " + std::to_wstring(frame + 1)
                             : L"its video has " + std::to_wstring(v.next) + L" frames";
                return nullptr;
            }
            if (FAILED(hr))
            {
                wchar_t code[16];
                swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
                why = L"its video could not be decoded (" + std::wstring(code) + L")";
                return nullptr;
            }
            if (v.reader && v.canSeek && v.rate > 0)
            {
                if (v.first < 0 && v.next == 0) v.first = time;
                // Which frame it is, by its time (after a seek)
                if (v.first >= 0) v.next = static_cast<int>(std::llround((time - v.first) * v.rate / 1e7));
            }
            if (v.next++ < frame) continue;
            if (v.next - 1 > frame && sought)
            {
                // Past it (a seek that went too far): from the start, frame after frame. (Without a seek:
                // a gap in the times, and this frame is the one.)
                if (!Restart(v))
                {
                    why = L"its video could not go to frame " + std::to_wstring(frame + 1);
                    return nullptr;
                }
                sought = false;
                continue;
            }
            if (!ToRgb(v, sample.Get()))
            {
                why = L"its video frames are in a format not known here";
                return nullptr;
            }
            v.last = frame;
            break;
        }
    }
    DcmDataset* out = NativeCopy(ds, v.rgb, 8);
    out->putAndInsertUint16(DCM_Rows, static_cast<Uint16>(v.h));
    out->putAndInsertUint16(DCM_Columns, static_cast<Uint16>(v.w));
    out->putAndInsertUint16(DCM_SamplesPerPixel, 3);
    out->putAndInsertString(DCM_PhotometricInterpretation, "RGB");
    out->putAndInsertUint16(DCM_PlanarConfiguration, 0);
    out->putAndInsertUint16(DCM_BitsAllocated, 8);
    out->putAndInsertUint16(DCM_BitsStored, 8);
    out->putAndInsertUint16(DCM_HighBit, 7);
    out->putAndInsertUint16(DCM_PixelRepresentation, 0);
    return out;
}

int VideoFrameCount(VideoFrames& v, double& frameTime)
{
    std::lock_guard<std::mutex> hold(v.lock);
    if (v.rate > 0) frameTime = 1000 / v.rate;
    return v.count;
}
