// Pixels for the explorer: the slices, the segmentations over them, the floating thumbnails and the mosaics
// (included by Shell.h, after the headers of the parts before it)
#pragma once

// One frame of a file: its values (monochrome: the modality values, or the stored ones) or its colors
struct Slice
{
    int width = 0, height = 0;
    bool mono = true, inverse = false;             // inverse: MONOCHROME1, the low values white
    int bits = 0;                                  // monochrome: of the stored values (negative: signed)
    std::vector<float> values;                     // monochrome
    std::vector<DWORD> rgb;                        // color: 0xRRGGBB
    double rowSpacing = 0, columnSpacing = 0;      // PixelSpacing (mm), 0 if not known
    double slope = 1, intercept = 0;               // the rescale of the file
    double center = 0, window = 0;                 // the first window of the header (width 0: none)
    double frameTime = 0;                          // a multi-frame file: ms from frame to frame (0: not known)
    bool isFloat = false;                          // decimal values (Float Pixel Data), in their units
};

bool FirstNumber(const std::string& value, double& out);

std::wstring DecodeSlice(const std::wstring& path, int frame, bool stored, Slice& s);

static const char* const SegSopClass = "1.2.840.10008.5.1.4.1.1.66.4";

static const char* const RtStructSopClass = "1.2.840.10008.5.1.4.1.1.481.3";

struct SegFrame
{
    double origin[3] = {}, row[3] = {}, column[3] = {}, dx = 1, dy = 1;
    int columns = 0, rows = 0, segment = 1;
    bool any = false;                               // (some of the mask is set)
    std::vector<Uint8> mask;                        // 1: in the segment
};

struct Contour
{
    std::vector<std::array<double, 3>> points;      // in the patient (mm)
    COLORREF color = 0;
    bool closed = true;
};

struct Overlay
{
    std::wstring path;
    bool rt = false, loaded = false, failed = false;
    std::string study;
    std::vector<SegFrame> frames;                   // a SEG
    std::map<int, COLORREF> colors;                 // (of its segments)
    double spacing = 0;                             // (between its frames along their normal: how near a plane must be)
    std::vector<Contour> contours;                  // an RT Structure Set
};

void LoadOverlay(Overlay& o);

// 8-bit pixels, 0xRRGGBB, top-down
struct Picture
{
    int width = 0, height = 0;
    std::vector<DWORD> px;
};

bool IsGray(const Picture& p);

void Percentiles(const std::vector<const float*>& planes, int w, const RECT& box, double p, double q, double& a, double& b);

Picture Shrink(const Slice& s, const RECT& box, double lo, double hi, int tw, int th);

Picture Mosaic(const std::vector<Picture>& tiles, int slots);

Picture Cascade(const std::vector<Picture>& tiles, DWORD background);

// What a thumbnail or an export shows: positions (in their order), each one its images in time order
// An image of a stack: its file, and its frame there (the frames of enhanced files are images of their own)
struct FrameRef
{
    std::wstring path;
    int frame = 0;
};

struct Stack
{
    std::vector<std::vector<FrameRef>> files;
    int frames = 1;                                 // one multi-frame file: its frames are the times
    double delay = 0;                               // ms from time to time (0: not known)
};

struct Thumb
{
    std::vector<Picture> frames;
    int delay = 100;                                // ms from frame to frame
    std::wstring caption, error;                    // what it shows of the stack; why there is nothing
};

std::vector<int> Spread(int first, int last, int count);

Thumb StackThumb(const Stack& st, int single, int tile, bool middle, const std::atomic<bool>* cancel);
