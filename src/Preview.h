// The preview of Explorer's preview pane: the image, its header, the folder's series, the DICOMDIR
// (included by Shell.h, after the headers of the parts before it)
#pragma once

HANDLE ThemedControls();

// A preview asks for its series in a thread of its own (a folder can have thousands of files), which
// posts `message` to its window when it has it
struct SeriesScan
{
    std::wstring path, folder;
    HWND window = nullptr;
    UINT message = 0;
    volatile LONG cancel = 0;
    std::vector<std::wstring> files;                // written before the message, read after it
    // And then the segmentations of its study near it (SEG, RT Structure Sets), posting `segmentsMessage`
    std::string study;
    UINT segmentsMessage = 0;
    std::vector<std::shared_ptr<Segmentation>> segmentations;
};

class PreviewHandler : public IInitializeWithItem, public IPreviewHandler, public IPreviewHandlerVisuals,
                       public IOleWindow, public IObjectWithSite
{
public:
    PreviewHandler();
    virtual ~PreviewHandler();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP Initialize(IShellItem* i, DWORD) override;

    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* rc) override;
    IFACEMETHODIMP SetRect(const RECT* rc) override;
    IFACEMETHODIMP DoPreview() override;
    IFACEMETHODIMP Unload() override;
    IFACEMETHODIMP SetFocus() override;
    IFACEMETHODIMP QueryFocus(HWND* focus) override;
    IFACEMETHODIMP TranslateAccelerator(MSG* msg) override;

    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override;
    IFACEMETHODIMP SetFont(const LOGFONTW* f) override;
    IFACEMETHODIMP SetTextColor(COLORREF color) override;

    IFACEMETHODIMP GetWindow(HWND* hwnd) override;
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override;

    IFACEMETHODIMP SetSite(IUnknown* s) override;
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override;

    HRESULT Host(HWND host, const RECT& rc);
    // (frames: of each file, which one of an enhanced multi-frame file it is; -1: the file as it is. space,
    // time: of each file, its place - a plane or a series - and its position, whose files are its times; the
    // jumps go through the places, delay ms apart)
    void ShowFiles(const std::vector<std::wstring>& files, int index, const std::vector<int>& frames = {},
                   const std::vector<int>& space = {}, const std::vector<int>& time = {}, int delay = 100,
                   const wchar_t* places = L"Plane")
    {
        spaceName = places;
        StopPoster();
        Play(false);
        Jump(false);
        spaceOf = space.size() == files.size() ? space : std::vector<int>();
        timeOf = time.size() == files.size() ? time : std::vector<int>();
        jumpDelay = delay;
        windowSet = false;
        zoom = 1;
        series = files;
        Places();
        seriesFrames = frames;
        if (std::none_of(seriesFrames.begin(), seriesFrames.end(), [](int f) { return f >= 0; })) seriesFrames.clear();
        slice = -1;
        group = -1;
        if (index >= 0 && index < static_cast<int>(files.size()))
        {
            GoSlice(index);
            return;
        }
        const bool had = !view.pixels.empty();
        view = Rendered();
        view.summary = L"No image";
        current.clear();
        Shown(had);
    }
    void ShowIndex(int index);
    bool Stepping() const;
    bool Running() const;
    void StartCine();
    bool Overlay() const;
    void SetGuides(std::vector<std::array<double, 4>> lines);
    void SetCrosshair(bool on, double column = 0, double row = 0);
    std::function<void(bool, double, double)> pointed;     // the mouse over the image (column, row), or off it
    // Hosted: segmentations over the image: a mask of its size (0xAARRGGBB, its colors by alpha already, top
    // down; none: empty) and contours (in the coordinates of its pixels)
    using Outline = SegOutline;
    void SetSegmentation(int width, int height, const std::vector<DWORD>& argb, std::vector<Outline> lines);
    void SetOverlay(bool on);
    // The segmentations over the image (and its overlay planes) shown or not: hosted, as the explorer says;
    // in Explorer, by the check box of its image bar (remembered)
    void SetSegmentsShown(bool on);
    std::function<void(bool)> running;              // the cine or the jumps started (true) or stopped
    const std::vector<DWORD>* Pixels(int& width, int& height) const;
    std::function<void(int)> moved;
    // The explorer's thumbnail of a node (0xRRGGBB pixels, its frames moving), in place of the first of
    // its files until the slider, the keys or the cine go to them
    struct PosterFrame { int width = 0, height = 0; std::vector<DWORD> px; };
    void ShowPoster(std::vector<PosterFrame> frames, int delay, const std::wstring& caption);
    bool DraggedWindow(std::wstring& file, double& center, double& width) const;

private:
    enum { TabColumns, TabHeader, TabTags, TabDirectory, TabCount };
    enum class Drag { None, Split, Window, Pan, Zoom, Track, TrackSpace };
    static constexpr UINT WM_READCOLUMNS = WM_APP + 1, WM_SERIES = WM_APP + 2, WM_SEGMENTS = WM_APP + 3;
    static constexpr UINT_PTR TimerCine = 1, TimerSettle = 2, TimerPoster = 3, TimerJump = 4;
    std::vector<PosterFrame> poster;                // hosted: the thumbnail shown before the files (none: empty)
    std::vector<std::array<double, 4>> guides;      // hosted: lines over the image (column, row to column, row)
    HBITMAP maskBitmap = nullptr;                   // the segmentations over the image
    int maskWidth = 0, maskHeight = 0;
    std::vector<Outline> outlines;
    bool segments = true;                           // shown (the segmentations, and the overlay planes)
    std::vector<std::shared_ptr<Segmentation>> nearby;   // in Explorer: the segmentations of its study near the file
    bool crosshair = false, pointing = false;       // (and the mouse over it, tracked to its leaving)
    double crossColumn = 0, crossRow = 0;
    std::vector<int> seriesFrames;                  // hosted: the frame of each file of `series` (empty: none)
    size_t posterFrame = 0;

    void PosterPixels();

    void StopPoster();
    static constexpr const wchar_t* Settings = L"Software\\explorer4dicoms\\Preview";

    LONG refs = 1;
    IShellItem* item = nullptr;
    IUnknown* site = nullptr;
    IPreviewHandler* previous = nullptr;            // the preview from before, for a file that is not DICOM
    LOGFONTW hostFont = {};
    bool hasFont = false;

    IPreviewHandlerVisuals* PreviousVisuals() const;

    bool StartPrevious(IStream* stream);
    std::wstring path, folder, note;
    std::wstring current;                           // the file shown: the path, or another one of its series
    std::shared_ptr<SeriesScan> scan;
    std::vector<std::wstring> series;               // the files of its series, in order (empty until read)
    int slice = -1;                                 // the one shown, in `series`
    bool overlay = true, playing = false, jumping = false;     // the cine (in time), the jumps (in space)
    // Hosted: of each file of `series`, its place in space (a plane, or a series) and in time (its position:
    // the files of one are its times); empty: each file a place of its own
    std::vector<int> spaceOf, timeOf;
    std::vector<std::vector<int>> spaces;           // the files of each place in space, in order
    std::map<int, std::vector<int>> times;          // the files of each position, in order
    std::vector<int> spaceIndex;                    // of each file, its place in `spaces`
    int jumpDelay = 100;                            // ms from place to place
    bool plain = false;                             // not DICOM, and nothing before: only a line saying so
    bool hosted = false;                            // in the DICOM explorer: the image alone
    bool stale = false;                             // its header and values are those of a slice shown before
    bool windowSet = false;                         // the window dragged: it stays from slice to slice
    HWND parent = nullptr, wnd = nullptr, edit = nullptr, list = nullptr, tree = nullptr;
    // A DICOMDIR: its records, and the files under each record that has them (a series, mostly): the
    // slider steps through those of the one chosen
    struct DirGroup { std::vector<std::wstring> files, labels; std::vector<HTREEITEM> items; };
    std::vector<DirNode> directory;
    std::vector<DirGroup> groups;
    int group = -1;
    bool selecting = false;                         // the tree selection changes from the code
    RECT bounds = {};
    RECT tabArea = {}, tabRects[TabCount] = {}, contentArea = {}, splitArea = {}, imageArea = {}, statusArea = {};
    RECT barArea = {}, playRect = {}, jumpRect = {}, trackRect = {}, spaceTrackRect = {}, overlayRect = {}, segmentsRect = {};
    const wchar_t* spaceName = L"Slice";            // what the places are (planes, series; the files of a folder: slices)
    int tab = TabColumns;
    int split = 450;                                // the share of the top panel, in thousandths
    Drag drag = Drag::None;
    POINT dragFrom = {};
    double dragCenter = 0, dragWidth = 0, dragViewX = 0, dragViewY = 0, dragZoom = 1;
    double zoom = 1, viewX = 0, viewY = 0;          // the zoom over the fit, and the image point in the middle
    bool filling = false;                           // the checks change from the code, not from a click
    Rendered view;
    HFONT font = nullptr, uiFont = nullptr, boldFont = nullptr;
    HBRUSH background = nullptr;
    COLORREF backColor = GetSysColor(COLOR_WINDOW), textColor = GetSysColor(COLOR_WINDOWTEXT);

    HBRUSH Background();

    COLORREF Dim(int percent) const;

    int Px(int n) const;

    HWND Content() const;

    static DWORD ReadDword(const wchar_t* key, const wchar_t* name, DWORD fallback);

    HRESULT CreateWindows();

    void FillTree();

    static LPARAM Code(int g, int i);
    static int GroupOf(LPARAM p);
    static int FileOf(LPARAM p);

    void AddNode(HTREEITEM under, const DirNode& n);

    void ExpandGroup(int g, HTREEITEM node);

    void Choose(LPARAM p);

    void Reveal(LPARAM p);

    void FillColumns();

    void FillValues(const std::wstring& file);

    void ReadColumns();

    void Toggle(int row);

    void CopyRows();

    void SelectTab(int t);

    void ShowContent();

    void Recolor();

    void Place();

    void Layout();

    void Paint(HDC dc);

    void PaintOverlay(HDC dc, const RECT& r);

    void PaintBar(HDC dc);

    int Count() const;
    int Position() const;
    void GoTo(int i);
    void Step(int delta);

    void GoFrame(int f);

    void GoSlice(int i);

    void Shown(bool had);

    bool HasSegments() const;

    void PaneSegments();

    void Settle();

    void Series();

    void Places();

    bool OverFrames() const;
    int TimeCount() const;
    int FirstTimes() const;      // (the thumbnail shown: the times of the first file)
    int SpaceCount() const;

    int TimeIndex() const;
    int SpaceIndex() const;

    void GoTime(int k);

    void GoSpace(int target);

    void StepTime(int delta);

    void StepSpace(int delta);

    void Play(bool on);

    void Jump(bool on);

    bool Key(WPARAM key);

    void TrackSpaceTo(POINT pt);

    void TrackTo(POINT pt);

    void MoveWindowTo(POINT pt);

    double Scale() const;

    void KeepView();

    void ZoomTo(POINT pt);

    void SetZoom(POINT pt, double next);

    void StartImageDrag(Drag what, POINT pt);

    void PanTo(POINT pt);

    void Redraw();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};
