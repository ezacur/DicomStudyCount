// The segmentations over the images: DICOM SEG (masks, frame by frame, each frame of a segment and with its
// plane) and RT Structure Sets (contours: points of the patient, plane by plane); those near a file, and what
// of them is in the plane of an image
// (included by Shell.h, after the headers of the parts before it)
#pragma once

static const char* const SegSopClass = "1.2.840.10008.5.1.4.1.1.66.4";

static const char* const RtStructSopClass = "1.2.840.10008.5.1.4.1.1.481.3";

// The plane of an image: its first pixel and the directions of its rows and columns (mm, of the patient),
// its pixel spacing, its size, and its thickness (0: not known)
struct ImagePlane
{
    double origin[3] = {}, row[3] = {}, column[3] = {}, dx = 1, dy = 1;
    int columns = 0, rows = 0;
    double thickness = 0;
};

// The plane from its orientation (6 numbers), its position (3), its pixel spacing (between rows, between
// columns), its size and its thickness; false without a size or a spacing
bool PlaneFrom(const double* orientation, const double* position, double dy, double dx, int columns, int rows,
               double thickness, ImagePlane& p);

bool PlaneOf(const Header& h, ImagePlane& p);

// A frame of a file: its own attributes, or else those of its functional groups (enhanced files)
bool PlaneOf(DcmDataset* ds, int frame, ImagePlane& p);

struct SegFrame
{
    ImagePlane plane;
    int segment = 1;
    bool any = false;                               // (some of the mask is set)
    std::vector<Uint8> mask;                        // 1: in the segment
};

struct Contour
{
    std::vector<std::array<double, 3>> points;      // in the patient (mm)
    COLORREF color = 0;
    bool closed = true;
};

struct Segmentation
{
    std::wstring path;
    bool rt = false, loaded = false, failed = false;
    std::string study;
    std::vector<SegFrame> frames;                   // a SEG
    std::map<int, COLORREF> colors;                 // (of its segments)
    double spacing = 0;                             // (between its frames along their normal: how near a plane must be)
    std::vector<Contour> contours;                  // an RT Structure Set
};

void LoadSegmentation(Segmentation& o);

// Something of a segmentation in the plane: a frame of a SEG with something in it, a contour of a structure set
bool SegmentsIn(const Segmentation& s, const ImagePlane& p);

// A contour over an image, in the coordinates of its pixels (columns, rows; 0 the middle of the first one)
struct SegOutline
{
    COLORREF color = 0;
    bool closed = true;
    std::vector<std::array<double, 2>> points;
};

// What of some segmentations is in the plane of an image: the masks of the SEG frames (0xAARRGGBB, top-down,
// of the size of the image, their colors faint inside and strong at their edge; empty: none) and the contours
// of the structure sets
void SegmentsOver(const std::vector<const Segmentation*>& all, const ImagePlane& p, std::vector<DWORD>& argb,
                  std::vector<SegOutline>& outlines);

// The segmentations of a study near a file, read: those of its folder, of the folders in it, and of the
// folders beside it (a study, a folder for each series)
std::vector<std::shared_ptr<Segmentation>> SegmentationsNear(const std::wstring& file, const std::string& study);
