// The DICOM explorer: its window, its settings, the loading, the drawing; and ExploreW (rundll32)
// (included by Shell.h, after the headers of the parts before it)
#pragma once

class DicomExplorer
{
    friend struct ExplorerTest;                     // (a test of the gathering, the checks and the exports, without the window)
public:
    DicomExplorer() = default;
    DicomExplorer(const DicomExplorer&) = delete;
    DicomExplorer& operator=(const DicomExplorer&) = delete;
    ~DicomExplorer();

    bool Create(const std::wstring& folder, int show, bool report = false);

    bool PreTranslate(MSG& m);

private:
    static constexpr const wchar_t* Settings = L"Software\\explorer4dicoms\\Explorer";
    static constexpr UINT WM_GATHERED = WM_APP + 20, WM_THUMB = WM_APP + 21, WM_EXPORTED = WM_APP + 22;
    static constexpr UINT_PTR TimerProgress = 1, TimerFilter = 2, TimerFind = 3, TimerFollow = 4, TimerJob = 5;
    static constexpr UINT_PTR TimerFrame = 1;      // (of the floating thumbnail)
    enum { IdPath = 100, IdBrowse, IdReload, IdReport, IdFind, IdCompact, IdIdentity, IdHover, IdFilter, IdNavigator, IdOverlay, IdRefLines, IdSegments };
    enum class Split { None, Tree, Top, Info, Unused };
    struct Row { Uint32 tag; std::wstring value; };     // of the full info
    static constexpr COLORREF ProblemColor = RGB(196, 90, 0);

    HWND wnd = nullptr, pathBox = nullptr, browse = nullptr, reload = nullptr, reportButton = nullptr, findLabel = nullptr, findBox = nullptr;
    HWND compactBox = nullptr, identityBox = nullptr, hoverBox = nullptr, navigatorBox = nullptr, overlayBox = nullptr, refBox = nullptr, segBox = nullptr, tree = nullptr, info = nullptr, filterLabel = nullptr, filterBox = nullptr;
    HWND full = nullptr, status = nullptr;
    int firstProblemRow = INT_MAX;                  // of the info: the rows of the problems, from this one
    std::vector<int> groupKind;                     // of the full info: the kind of each group (by its id: its level, 100 what varies)
    std::vector<std::wstring> groupTitle;           // and its title (after its triangle)
    std::set<int> folded;                           // the kinds of group folded (they stay so from node to node)
    // The floating thumbnail of the node under the mouse in the tree
    HWND popup = nullptr;
    bool hoverPreview = true, hoverArmed = false;
    HTREEITEM hoverItem = nullptr;
    int popupNode = -1, popupFrame = 0;
    std::unique_ptr<ThumbWorker> worker;
    std::unordered_map<int, std::shared_ptr<Thumb>> thumbs;     // by node, for this gathering
    std::unordered_map<int, std::shared_ptr<Thumb>> posters;    // the larger ones of the image panel
    // The navigator, and the planes of the images: each one once (the images of a position over each other)
    Navigator navigator;
    std::vector<NavPlane> navPlanes;
    bool planesMade = false;
    int navigated = -1;                             // the planes the navigator shows: of this node (all, 0)
    int navPatient = -1;                            // and the patient of the node chosen (another one: framed again)
    std::vector<int> navScene;                      // the plane of each one it draws
    std::vector<int> navSceneOf;                    // and where each plane is among them (-1: not drawn)
    int currentFile = -1;                           // the file the image panel shows (-1: a thumbnail, or none)
    Plot plot;                                      // an attribute along the files of the node chosen
    Uint32 plotTag = 0;                             // (which one)
    std::vector<int> plotFiles;                     // (its files, in order)
    bool refLines = true;                           // reference lines: where the planes of the node under the mouse cut it
    bool showSegments = true;                       // segmentations (SEG, RT Structure Sets, overlay planes) over the images
    std::vector<Segmentation> segmentations;        // the SEG and RT Structure Sets in the folder (read when an image of their study is shown)
    // The marks of the images in the tree, for what they have: on an image its word, on the nodes above it
    // how many of theirs have it; and their check boxes in a color of their own (the blue of the box made
    // of its channels: "from" says which one goes to red, green and blue; an empty box filled light). A
    // node with images of two marks: half its box of each. (The state images of a tree are 15 at most:
    // 1 + 3 for each combination of the marks with a color)
    enum TreeMark { Segmented, Marks };
    struct MarkKind { const wchar_t* image; const wchar_t* counted; int from[3]; DWORD light; };   // (light: 0xRRGGBB)
    static constexpr MarkKind MarkKinds[Marks] = {
        { L"seg", L"segmented", { 0, 2, 1 }, 0xAAE6AA },            // (green: the images with a segmentation in their plane, or overlay planes, 60xx, of their own)
    };
    static_assert(1 + 3 * (1 << Marks) <= 15, "the state images of a tree are 15 at most");
    std::vector<size_t> markCount[Marks];           // each node: how many of its images have each mark
    std::vector<size_t> derivedCount;               // each node: how many of its files are derived
    HFONT italicFont = nullptr, boldItalicFont = nullptr;
    int marked = -1;                                // the node under the mouse in the tree, its planes framed there
    bool markTracking = false;                      // (of the mouse leaving the tree)
    std::vector<int> lit;                           // the nodes of the plane under the mouse there, marked in the tree
    std::vector<size_t> onCount;                    // each node: how many of its files are drawn in the navigator (FileState::on)
    // The unused tree, under the tree: the parts of it put aside (not drawn in the navigator). Both show the
    // same nodes: the tree those with files in use, the unused one those with files put aside.
    HWND unusedLabel = nullptr, unusedTree = nullptr;
    HWND hoverTree = nullptr;                       // the tree the mouse is over
    std::vector<size_t> usedCount;                  // each node: how many of its files are not put aside (FileState::unused)
    std::vector<HTREEITEM> unusedItem;              // each node: its item in the unused tree
    std::vector<char> unusedFilled;                 // its shown children are there
    std::vector<signed char> openMemory;            // each node: open (1) or not (0) when last in the tree (-1: never), for Compact
    int unusedShare = 250;                          // (of the height of the trees, in thousandths)
    RECT unusedSplit = {}, trees = {};              // the bar between the trees, and where both are
    unsigned generation = 0;
    // An export or a report being written
    std::unique_ptr<ExportJob> job;
    bool reportOnLoad = false;                      // the report of the folder once it is read (rundll32 ...,Explore /report <folder>)
    HFONT font = nullptr, boldFont = nullptr;
    HICON icon = nullptr, smallIcon = nullptr;
    PreviewHandler* preview = nullptr;
    RECT imageRect = {}, treeSplit = {}, topSplit = {}, infoSplit = {};
    Split split = Split::None;
    int treeWidth = 430, infoWidth = 380, topShare = 560;  // (widths at 96 dpi; the share in thousandths)

    std::wstring root, summary;                     // the folder, and what was found in it
    std::unique_ptr<Gathering> data;
    bool ready = false, compact = true, hideIdentity = true;
    std::vector<GatherNode> nodes;
    // Each file (of data->files): what the explorer knows of it, made with the tree (Build)
    struct FileState
    {
        int node = -1;                              // its image node
        int rank = 0;                               // its place in the order of the images
        bool derived = false;                       // a derived image (ImageType DERIVED), not an original one
        Uint32 frame = 0;                           // its frame of reference (an id of the pool, +1; 0: none)
        bool on = true;                             // drawn in the navigator (the check boxes of the tree)
        bool unused = false;                        // put aside (the unused tree)
        int plane = -1;                             // its plane in the navigator (-1: none, without a position; or not made yet)
    };
    std::vector<FileState> fileState;
    int selected = -1;                              // the node chosen
    int sliderNode = -1;                            // the node whose files the slider goes through
    bool following = false;                         // the tree selection moves with the slider
    std::wstring findText, filterText;
    std::wregex findRegex, filterRegex;
    bool findIsRegex = false, filterIsRegex = false;

    // ----- settings

    static DWORD ReadSetting(const wchar_t* name, DWORD fallback);
    static void WriteSetting(const wchar_t* name, DWORD value);
    static std::wstring ReadText(const wchar_t* name);
    void SaveSettings();

    int Px(int n) const;

    static std::wstring TextOf(HWND h);

    // ----- the window

    void CreateControls();

    void Refont();

    void Layout();

    void SetStatus(const std::wstring& text);

    // ----- loading

    void Browse();

    void Stop();

    void Load(std::wstring folder);

    void Clear();

    void Progress();

    void Finished();

    // ----- the tree of DICOMgather.m

    int Add(Level level, int parent, std::vector<int> files);

    // The files grouped by a key, in the order in which the groups appear
    template <class K>
    static std::vector<std::vector<int>> GroupBy(const std::vector<int>& in, K key)
    {
        std::vector<std::vector<int>> groups;
        std::unordered_map<std::string, size_t> at;
        for (int f : in)
        {
            auto r = at.emplace(key(f), groups.size());
            if (r.second) groups.emplace_back();
            groups[r.first->second].push_back(f);
        }
        return groups;
    }

    void Build();

    // ----- the checks: what may be wrong in the series

    void Problem(int n, const std::wstring& flag, const std::wstring& text);

    const std::wstring* AttributeOf(int f, Uint32 tag) const;
    std::vector<Uint32> OverlayGroupsOf(int f) const;

    void Check();

    std::vector<std::wstring> ProblemsUnder(int n, size_t most) const;

    void AddSeries(int study, const std::vector<int>& rg);

    size_t FilesOf(int n) const;

    std::wstring HowMany(int n) const;

    std::wstring Label(int i) const;

    std::wstring KindOf(int n) const;

    std::wstring Plain(int i) const;

    std::wstring Size(int o) const;

    bool Skipped(int c) const;

    void ComputeShown();

    bool InTree(int n, bool unused) const;

    bool HasShown(int n, bool unused) const;

    void InsertUnused(int n);

    HTREEITEM UnusedItemOf(int n);

    void Refill(bool unused);

    void MoveUnused(int n, bool aside);

    void UnusedLabel();

    void Insert(int n);

    HTREEITEM ItemOf(int n);

    void FillTree();

    void Recompact();

    // ----- a node chosen

    void Choose(int n);

    void ShowPoster(int n);

    // ----- the navigator

    void MakePlanes();

    void Navigate(int n);

    int StateOf(int n) const;

    void CheckImages();

    int CheckOf(int n) const;

    void Toggle(int n);

    // The plane of a file: where its first pixel is, the directions of its rows and columns, its pixel spacing, its thickness
    bool PlaneOfFile(int f, ImagePlane& g) const;

    void ShownFile(int i);

    void Guides(int n);

    void MarkSegmented();
    void SetMark(TreeMark m, const std::vector<char>& with);

    void Segments();

    void Pointed(bool on, double column, double row);

    void Aimed(bool on, const double* from, const double* toward);

    void Mark(HWND t, POINT pt);

    void Pick(int i);

    void Light(int i);

    void Live(int i);

    void Followed(int i);

    void FillInfo(int n);

    const Attributes& Common(int n);

    static const std::pair<Uint32, Uint32>* Find(const Attributes& a, Uint32 tag);

    std::wstring Value(int n, Uint32 tag);

    std::wstring Shown(Uint32 tag, const std::wstring& value) const;

    // ----- the full info

    static bool Matches(const std::wstring& text, const std::wstring& what, const std::wregex& re, bool isRegex);

    static void Compile(const std::wstring& text, std::wregex& re, bool& isRegex);

    void FillFull(int n);

    std::vector<Row> Varying(int n);

    static std::wstring GroupTitle(const std::wstring& title, bool fold);

    int TriangleAt(HWND h, POINT pt) const;

    static LRESULT CALLBACK FullProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref);

    static LRESULT CALLBACK HeaderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref);

    // ----- the plot

    Uint32 RowTag(int row) const;

    void PlotRow(int row);

    void FillPlot(int n);

    void PlotPicked(int i);

    void FullMenu(LPARAM lp);

    // The Tag column of the full info: folded (a narrow one, its triangle alone) or open
    bool tagOpen = false;
    void TagColumn(bool open);

    // ----- find in the tree

    std::vector<int> Order() const;

    bool Hit(int n) const;

    // Marked in a tree: the nodes found, or for those in a closed branch the node shown that holds them
    std::vector<char> findHits, findMarks[2];
    bool foundStale = true;
    bool Found(int n, bool unused);
    void MarkFound();
    void FoundAgain();

    void FindChanged();

    void FindNext(bool back);

    // ----- context menu, copying

    std::vector<std::wstring> PathsOf(int n) const;

    void CopyPaths(int n);

    void CopyRows(HWND list, bool selection);

    void TreeMenu(LPARAM lp, bool unused);

    // ----- the floating thumbnail of the node under the mouse

    static LRESULT CALLBACK TreeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref);

    int NodeAt(POINT pt, HTREEITEM* where = nullptr) const;

    void HoverMove(POINT pt);

    void HoverShow(POINT pt);

    void HidePopup();

    Stack StackOf(int n) const;

    int MainOrientation(int series) const;

    void AskThumb(int n, bool big = false);

    void Thumbnailed(ThumbDone* d);

    std::wstring PopupText() const;

    int PopupScale() const;

    void PlacePopup();

    void PaintPopup(HDC dc);

    static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // ----- exports and the report

    static std::wstring Clean(std::wstring s);

    std::wstring SeriesName(int r) const;

    int SeriesOf(int n) const;

    std::wstring FolderName(int n) const;

    std::wstring OrientationName(int o) const;

    std::wstring PositionName(int p) const;

    std::wstring ImageName(int i) const;

    void NiftiUnit(ExportUnit& u, const std::vector<int>& positions) const;

    void Plan(int n, bool nifti, const std::wstring& folder, ExportJob& j);

    void PlanOrientation(int o, bool nifti, const std::wstring& base, ExportJob& j);

    void PlanPosition(int p, bool nifti, const std::wstring& base, ExportJob& j);

    void PlanImage(int i, bool nifti, const std::wstring& base, ExportJob& j);

    std::wstring PickFolder(const wchar_t* title, const wchar_t* setting);

    void Export(int n, bool nifti);

    void StartJob(std::unique_ptr<ExportJob> j);

    void JobProgress();

    void StopJob();

    void JobDone();

    // ----- the report

    static std::wstring Html(const std::wstring& s);

    std::wstring ReportPath(int n) const;

    void Report(int n, std::wstring path = std::wstring());

    void ExpandAll(int n, bool open);

    // ----- drawing

    static COLORREF LevelColor(int level);

    LRESULT Notify(NMHDR* h);

    Split SplitAt(POINT pt) const;

    void DragTo(POINT pt);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};
