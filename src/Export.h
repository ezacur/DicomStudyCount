// The exports of the explorer (PNG, animated GIF, NIfTI) and the worker of its thumbnails
// (included by Shell.h, after the headers of the parts before it)
#pragma once

struct NiftiHeader
{
    int sizeof_hdr = 348;
    char data_type[10] = {}, db_name[18] = {};
    int extents = 0;
    short session_error = 0;
    char regular = 'r', dim_info = 0;
    short dim[8] = {};
    float intent_p1 = 0, intent_p2 = 0, intent_p3 = 0;
    short intent_code = 0, datatype = 0, bitpix = 0, slice_start = 0;
    float pixdim[8] = {};
    float vox_offset = 352, scl_slope = 1, scl_inter = 0;
    short slice_end = 0;
    char slice_code = 0, xyzt_units = 0;
    float cal_max = 0, cal_min = 0, slice_duration = 0, toffset = 0;
    int glmax = 0, glmin = 0;
    char descrip[80] = {}, aux_file[24] = {};
    short qform_code = 0, sform_code = 0;
    float quatern_b = 0, quatern_c = 0, quatern_d = 0, qoffset_x = 0, qoffset_y = 0, qoffset_z = 0;
    float srow_x[4] = {}, srow_y[4] = {}, srow_z[4] = {};
    char intent_name[16] = {}, magic[4] = { 'n', '+', '1', 0 };
};

// One file to write: a PNG, an animated GIF or a NIfTI volume
struct ExportUnit
{
    enum Kind { Png, Gif, Nifti } kind = Png;
    std::wstring out;
    Stack stack;
    bool overTime = false;                          // a GIF: through the times (a cine), or through the positions
    // NIfTI: where the positions are (DICOM, mm), the plane, the spacings
    bool placed = false, sameRescale = true;
    double orientation[6] = {};
    std::vector<std::array<double, 3>> positions;
    double thickness = 0, timeStep = 0;
    std::string description;
};

std::wstring WritePictures(IWICImagingFactory* factory, const ExportUnit& u, bool dragged, double center, double width,
                                  const std::atomic<bool>& cancel, std::atomic<long>& done, bool& wrote);

std::wstring WriteNifti(const ExportUnit& u, const std::atomic<bool>& cancel, std::atomic<long>& done, bool& wrote);

std::wstring FreePath(const std::wstring& path);

// The exports and the report, in a thread of their own; the window is told when they are over
struct ExportJob
{
    std::vector<ExportUnit> units;
    bool dragged = false;                           // the window dragged in the preview, for all of them
    double center = 0, width = 0;
    std::wstring report, reportPath;                // a report: its HTML, "{{thumb N}}" for the thumbnail of stacks[N]
    std::vector<Stack> stacks;
    HWND window = nullptr;
    UINT message = 0;
    std::atomic<bool> cancel{ false };
    std::atomic<long> done{ 0 };
    long total = 0;
    std::vector<std::wstring> written, notes;       // (written before the message)
    long withoutImage = 0;                          // orientations left out: no pixels in them
    std::thread thread;
};

void RunExport(ExportJob* j);

// The thumbnails of the tree, one at a time in a thread of their own: only the last one asked for (one
// asked before, still being made, is dropped)
struct ThumbAsk
{
    int node = -1;
    unsigned generation = 0;
    bool big = false;                               // for the image panel (larger), not for the floating one
    enum Kind { Single, Cascade, Grid } kind = Single;
    std::vector<Stack> stacks;                      // Single: one; Cascade: the orientations; Grid: the series
    DWORD background = 0;
};

struct ThumbDone
{
    int node = -1;
    unsigned generation = 0;
    bool big = false;
    Thumb thumb;
};

// Two at a time can be waiting: the one of the image panel (first) and the floating one; another one of
// the same kind replaces it, and drops it if it is being made
class ThumbWorker
{
public:
    ThumbWorker(HWND w, UINT m);
    ThumbWorker(const ThumbWorker&) = delete;
    ThumbWorker& operator=(const ThumbWorker&) = delete;
    ~ThumbWorker();
    void Ask(ThumbAsk a);

private:
    HWND window;
    UINT message;
    std::mutex lock;
    std::condition_variable wake;
    std::unique_ptr<ThumbAsk> pending[2];          // big, small
    int running = -1;
    std::atomic<bool> stale{ false };
    bool quit = false;
    std::thread thread;

    void Run();

    Thumb Make(const ThumbAsk& a);
};
