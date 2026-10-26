// The plot of an attribute along the files of a node, under the full info of the explorer
// (included by Shell.h, after the headers of the parts before it)
#pragma once

class Plot
{
public:
    std::function<void(int)> picked;            // a click on a file (its index here)
    std::function<void()> closed;               // its x

    ~Plot();

    bool Create(HWND parent);
    bool Visible() const;
    void Show(bool on);
    void Move(const RECT& r);

    void Set(std::wstring what, std::vector<std::wstring> values, std::vector<std::wstring> names, int current);
    void SetCurrent(int i);

private:
    HWND wnd = nullptr;
    HFONT font = nullptr;
    UINT fontDpi = 0;
    bool shown = false, numeric = true;
    std::wstring title;
    std::vector<std::wstring> text, files, categories;
    std::vector<std::vector<double>> numbers;
    std::vector<int> category;
    size_t parts = 0;
    int now = -1, hover = -1;
    RECT area = {}, closeBox = {};

    int Px(int n) const;

    void Range(double& lo, double& hi) const;
    int X(int i) const;
    int Y(double v, double lo, double hi) const;
    int At(int x) const;     // (the file nearest a point)

    void Paint(HDC target);

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};
