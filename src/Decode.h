// The decoding of DICOM images with DCMTK (and OpenJPEG, libjxl, deflated and float frames, Philips private CT), the overlay, the DICOMDIR
// (included by Shell.h, after the headers of the parts before it)
#pragma once

void InitDcmtk();

struct VideoFrames;

// The decoded file of the preview, kept to change its frame, and its window (brightness / contrast)
// while the mouse drags over it. Monochrome: the window of DCMTK, on the values of the pixels. Color:
// the same on the rendered RGB (0..255), from center 127.5 and width 255 (the image as it is).
struct Imaging
{
    std::unique_ptr<DcmFileFormat> file;
    std::unique_ptr<DcmDataset> decoded;           // JPEG 2000, decoded by OpenJPEG
    std::shared_ptr<VideoFrames> video;            // a video, as far as it is decoded
    std::unique_ptr<DicomImage> image;             // destroyed first: it reads the two above
    E_TransferSyntax xfer = EXS_Unknown;
    bool cutJpeg = false;                          // a JPEG the file ends in (DecodeCutJpeg)
    bool mono = false;
    std::vector<DWORD> color;
    double center = 127.5, width = 255, center0 = 127.5, width0 = 255, low = 0, high = 255;
    // Overlay planes of the frame (60xx: in OverlayData, or in the high bits of the pixels), as one of
    // its size: 0 nothing, k the plane k - 1 (empty: none)
    std::vector<Uint8> planes;
};

// A record of a DICOMDIR (patient, study, series, image...): its line in the tree, the file it refers
// to (relative to the DICOMDIR) and the records under it
struct DirNode
{
    std::string type;
    std::wstring text, file;
    long number = 0;                               // a series: its number; a file: its instance number
    std::vector<DirNode> children;
};

// What the preview shows: one frame as 32-bit BGR rows (top-down), a summary line, the header twice
// (one line per element: with the tags "(gggg,eeee)" and with their dictionary names) and the text of
// the overlay. The thumbnail: the first frame, and the study and series for its badge.
struct Rendered
{
    int width = 0, height = 0;
    std::vector<DWORD> pixels;
    std::wstring summary, tags, names;
    std::string studyUid, seriesNumber;
    int frames = 1, frame = 0;
    double frameTime = 0;                          // ms from frame to frame (the cine), 0 if not known
    double aspect = 1;                             // the height of a pixel for its width (PixelSpacing: rows / columns)
    std::wstring corners[3];                       // the overlay: top left, top right, bottom left (lines)
    std::wstring sides[4];                         // the patient directions at the left, top, right, bottom
    std::vector<DirNode> directory;                // a DICOMDIR: its patients
    std::shared_ptr<Imaging> imaging;              // only in the preview (the file, even without image)
};

void Rewindow(Rendered& r);

std::wstring ValueOf(DcmElement* e, const std::string& charset);

void DumpHeader(DcmFileFormat& ff, std::wstring& tags, std::wstring& names);

std::wstring DateTimeText(const std::string& date, const std::string& time);

bool IsJpeg2000(E_TransferSyntax x);

DcmDataset* DecodeJpeg2000(DcmDataset* ds, bool multiframe, int frame);

bool IsJpegXl(E_TransferSyntax x);

DcmDataset* DecodeCutJpeg(DcmDataset* ds, int frame);

DcmDataset* DecodeJpegXl(DcmDataset* ds, bool multiframe, int frame);

bool IsPmsct(DcmFileFormat& ff);

DcmDataset* DecodePmsct(DcmDataset* ds);

bool FrameBytes(DcmDataset* ds, bool multiframe, int frame, std::vector<Uint8>& out);

bool UnindexedJpegFrames(DcmDataset* ds, E_TransferSyntax xfer);

DcmDataset* DecodeUnindexedJpegFrame(DcmDataset* ds, E_TransferSyntax xfer, int frame);

DcmDataset* NativeCopy(DcmDataset* ds, const std::vector<Uint8>& bytes, Uint16 allocated);

DcmDataset* DecodeEncapsulatedFrame(DcmDataset* ds, bool multiframe, int frame);

// (cut: set when its pixels end before the image does, the rest zeros: data that is not in the file)
DcmDataset* RepairedImage(DcmDataset* ds, E_TransferSyntax& xfer, bool* cut = nullptr);

std::unique_ptr<DicomImage> RenderAgain(DcmDataset* ds, E_TransferSyntax xfer, unsigned long flags, unsigned long frame,
                                        std::unique_ptr<DcmDataset>& kept, bool* cut = nullptr);

bool RecoverPixelData(DcmDataset* ds, const char* data, size_t size, bool* cut = nullptr);

bool TrimLastFragment(DcmDataset* ds, const char* data, size_t size);
bool TrimLastFragment(DcmDataset* ds, const std::wstring& path);
unsigned long PartialAccess(DcmDataset* ds, E_TransferSyntax xfer);
void OrderBigEndianWords(DcmDataset* ds);
void OrderGePixels(DcmFileFormat& ff);

DcmDataset* DecodeDeflatedFrame(DcmDataset* ds, bool multiframe, int frame);

bool IsBitFrames(DcmDataset* ds, E_TransferSyntax xfer);

DcmDataset* DecodeBitFrame(DcmDataset* ds, int frame);

bool FloatFrame(DcmDataset* ds, int frame, std::vector<double>& values);

bool HasFloatPixels(DcmDataset* ds);

std::wstring NoImage(DcmFileFormat& ff);

std::wstring ShowFrame(Rendered& r, int frame, bool keep);

// The preview reads the whole file once (Everything: the header too); stepping through a series, each
// file without its header (Image: it is written when the stepping stops). The thumbnail: only pixels.
enum class Render { Thumbnail, Image, Everything };

Rendered RenderDicom(const std::vector<char>& data, Render what = Render::Everything);

bool ReadPath(const std::wstring& path, std::vector<char>& data);

HRESULT ReadAll(IStream* stream, std::vector<char>& data);
