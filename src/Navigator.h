// The navigator of the explorer: the images as planes where they are in the patient, in 3-D
// (included by Shell.h, after the headers of the parts before it)
#pragma once

// An image in the patient (mm, DICOM's LPS): its corners, at the edges of its pixels
struct NavPlane { float p[4][3]; };             // top left, top right, bottom left, bottom right

class Navigator
{
public:
    ~Navigator();

    bool Create(HWND parent);

    bool Visible() const;

    void Show(bool on);

    void Move(const RECT& r);

    void Set(std::vector<NavPlane> p, std::vector<char> h, int on, Picture picture, std::wstring text, std::wstring more, bool fit);

    void Clear();

    void SetGuides(std::vector<std::array<float, 6>> lines);
    void SetPoint(bool on, const double* p = nullptr);
    // Contours of RT Structure Sets, in the patient, each in its color
    struct Line3 { COLORREF color = 0; bool closed = true; std::vector<std::array<float, 3>> points; };
    void SetContours(std::vector<Line3> lines);
    // The mouse over the view: the line of sight under it (a point, and its direction), or off it
    std::function<void(bool, const double*, const double*)> aimed;

    void SetLive(int i, int w, int h, const std::vector<DWORD>* px);

    std::function<void(int)> hovered;           // the mouse went over a plane (its index in Set), or off them (-1)
    std::function<void(int)> picked;            // a double click on a plane (its index in Set)
    static constexpr COLORREF HoverColor = RGB(0, 90, 230);

    void SetHoverText(std::wstring text);

    void SetMarks(std::vector<int> which);

private:
    HWND wnd = nullptr;
    HBITMAP bitmap = nullptr;                   // the picture, for PlgBlt
    HFONT font = nullptr;
    UINT fontDpi = 0;
    std::vector<NavPlane> planes;
    std::vector<char> hot;
    int textured = -1;
    Picture texture;
    std::wstring caption, detail;
    double center[3] = {}, radius = 1, zoom = 1, panX = 0, panY = 0;
    double azimuth = -37.5, elevation = 30;     // (degrees, view(3) of MATLAB)
    POINT last = {}, down = {};                 // (where the mouse is, and where it was pressed)
    bool rotating = false, moving = false, zooming = false;
    bool shown = false;
    std::vector<POINT> corners;                 // of the planes in the window, as last drawn (4 each)
    int hover = -1;                             // the plane under the mouse
    int live = -1, liveWidth = 0, liveHeight = 0;   // the image of the cine or the jumps, on this plane
    std::vector<std::array<float, 6>> guides;   // reference lines (from, to)
    std::vector<Line3> contours;                // of the segmentations
    bool pointOn = false;                       // the point under the mouse in the image
    double point[3] = {};
    HBITMAP liveBitmap = nullptr;
    std::vector<int> marks;                     // planes of the node under the mouse in the tree
    std::wstring hoverText;                     // what it is
    bool tracking = false;                      // (of the mouse leaving)

    int Px(int n) const;

    void Axes(double r[3][3]) const;

    // The patient as blocks (LPS: x to its left, y to its back, z to its head), standing, its nose to the
    // front: head, nose, body, arms, legs. Its sides seen from the view, the farther ones first; each
    // one kept (where it is in the window, which way it faces) for the clicks.
    struct Side { POINT p[4]; double normal[3]; };
    std::vector<Side> figure;                   // as drawn: the last one on top

    void Figure(HDC dc, const double r[3][3], int w, int h);

    const Side* SideAt(POINT pt) const;

    void LookFrom(const double n[3]);

    int PlaneAt(POINT pt) const;

    void Aim(POINT pt);

    void Hover(int i);

    void Paint(HDC target);

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};
