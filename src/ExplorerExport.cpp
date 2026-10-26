// The DICOM explorer: the exports (PNG, animated GIF, NIfTI)
#include "Shell.h"

// ----- exports and the report

// A name for a file or a folder: what Windows does not take, out; not too long
std::wstring DicomExplorer::Clean(std::wstring s)
{
    for (auto& c : s) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    std::wstring out;
    for (wchar_t c : s) if (!(c == L' ' && (out.empty() || out.back() == L' '))) out += c;
    if (out.size() > 90) out.resize(90);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out.empty() ? L"_" : out;
}

// "S005 MR cine_sa": the number, modality and description of a series
std::wstring DicomExplorer::SeriesName(int r) const
{
    const Gathered& g = data->files[nodes[r].files[0]];
    wchar_t number[32] = L"S";
    if (!g.seriesNumber.empty()) swprintf_s(number, L"S%03ld", atol(g.seriesNumber.c_str()));
    std::wstring s = number;
    if (!g.modality.empty()) s += L" " + Widen(g.modality);
    if (!g.seriesDescription.empty()) s += L" " + Widen(g.seriesDescription, g.charset);
    return Clean(s);
}

int DicomExplorer::SeriesOf(int n) const
{
    while (n > 0 && nodes[n].level != Level::Series) n = nodes[n].parent;
    return n;
}

// The folder of a patient, a study (or all): their line, without the counts
std::wstring DicomExplorer::FolderName(int n) const
{
    if (nodes[n].level == Level::All) return Clean(root.substr(root.find_last_of(L'\\') + 1));
    std::wstring s = Plain(n);
    const size_t open = s.rfind(L"   (");
    if (open != std::wstring::npos) s.resize(open);
    return Clean(s);
}

// The name of an orientation in its series: the series, and its plane when there are more
std::wstring DicomExplorer::OrientationName(int o) const
{
    const int r = nodes[o].parent;
    std::wstring s = SeriesName(r);
    if (nodes[r].children.size() > 1)
    {
        Header h;
        h.imageOrientation = data->files[nodes[o].files[0]].orientationText;
        std::wstring plane = PlaneName(h);
        const auto& c = nodes[r].children;
        s += L" - " + (plane.empty() ? L"orientation" : plane) + L" " + std::to_wstring(std::find(c.begin(), c.end(), o) - c.begin() + 1);
    }
    return s;
}

std::wstring DicomExplorer::PositionName(int p) const
{
    const Gathered& g = data->files[nodes[p].files[0]];
    wchar_t z[40] = L"";
    if (g.hasZ) swprintf_s(z, L" Z%.2f", g.z);
    else swprintf_s(z, L" P%zu", static_cast<size_t>(std::find(nodes[nodes[p].parent].children.begin(), nodes[nodes[p].parent].children.end(), p) -
                                                        nodes[nodes[p].parent].children.begin()) + 1);
    return OrientationName(nodes[p].parent) + z;
}

std::wstring DicomExplorer::ImageName(int i) const
{
    const Gathered& g = data->files[nodes[i].files[0]];
    std::wstring s = PositionName(nodes[i].parent);
    if (!g.instance.empty()) s += L" I" + Widen(g.instance);
    else s += L" " + Clean(g.path.substr(g.path.find_last_of(L'\\') + 1));
    return s;
}

// The NIfTI of an orientation, a position (a cine of one slice) or an image: where its positions are
void DicomExplorer::NiftiUnit(ExportUnit& u, const std::vector<int>& positions) const
{
    const std::vector<Gathered>& files = data->files;
    const Gathered& g = files[nodes[positions[0]].files[0]];
    u.kind = ExportUnit::Nifti;
    u.placed = g.oriented;
    memcpy(u.orientation, g.orientation, sizeof(u.orientation));
    for (int p : positions)
    {
        const Gathered& h = files[nodes[p].files[0]];
        u.placed = u.placed && h.placed;
        u.positions.push_back({ h.position[0], h.position[1], h.position[2] });
    }
    double v = 0;
    const std::wstring* t = AttributeOf(nodes[positions[0]].files[0], 0x00180088);       // SpacingBetweenSlices
    if (!t || !FirstNumber(Narrow(*t), v) || v <= 0) t = AttributeOf(nodes[positions[0]].files[0], 0x00180050);
    if (t && FirstNumber(Narrow(*t), v) && v > 0) u.thickness = v;
    u.timeStep = u.stack.delay;
    // The rescale of all of them the same: the stored values, and it in the header
    for (Uint32 tag : { 0x00281053u, 0x00281052u })
    {
        const std::wstring* first = AttributeOf(nodes[positions[0]].files[0], tag);
        for (int p : positions)
            for (int f : nodes[p].files)
            {
                const std::wstring* other = AttributeOf(f, tag);
                if ((first == nullptr) != (other == nullptr) || (first && *first != *other)) u.sameRescale = false;
            }
    }
}

// The files of a node to write, into a folder: a study (a patient, all) as folders of its own, its
// series as files there. Images: an image as a PNG (its frames, a GIF); several images of a position
// (a cine) as a GIF; an orientation of one image a position as a GIF through them, of more each of its
// positions as their own GIF. NIfTI: an orientation (a position, an image) as a volume.
void DicomExplorer::Plan(int n, bool nifti, const std::wstring& folder, ExportJob& j)
{
    const GatherNode& node = nodes[n];
    const std::wstring at = folder + L"\\";
    switch (node.level)
    {
    case Level::All:
    case Level::Patient:
    case Level::Study:
        for (int c : node.children) Plan(c, nifti, nodes[c].level == Level::Series ? folder : at + FolderName(c), j);
        return;
    case Level::Series:
        for (int o : node.children) PlanOrientation(o, nifti, at + OrientationName(o), j);
        return;
    case Level::Orientation:
        PlanOrientation(n, nifti, at + OrientationName(n), j);
        return;
    case Level::Position:
        PlanPosition(n, nifti, at + PositionName(n), j);
        return;
    case Level::Image:
        PlanImage(n, nifti, at + ImageName(n), j);
        return;
    }
}

void DicomExplorer::PlanOrientation(int o, bool nifti, const std::wstring& base, ExportJob& j)
{
    // (a report, a PDF, a presentation state: nothing to show)
    if (data->files[nodes[o].files[0]].rows == 0) { j.withoutImage++; return; }
    const auto& positions = nodes[o].children;
    if (positions.size() == 1) { PlanPosition(positions[0], nifti, base, j); return; }
    ExportUnit u;
    u.stack = StackOf(o);
    const size_t times = nodes[positions[0]].files.size();
    const bool even = std::all_of(positions.begin(), positions.end(), [&](int p) { return nodes[p].files.size() == times; });
    if (nifti)
    {
        if (!even)
        {
            j.notes.push_back(base.substr(base.find_last_of(L'\\') + 1) + L": not written, its positions have different numbers of images");
            return;
        }
        NiftiUnit(u, positions);
        u.out = base + L".nii";
        u.description = Narrow(base.substr(base.find_last_of(L'\\') + 1));
        j.total += static_cast<long>(nodes[o].files.size());
        j.units.push_back(std::move(u));
        return;
    }
    if (times == 1 && even)
    {
        u.kind = ExportUnit::Gif;
        u.out = base + L".gif";
        j.total += static_cast<long>(positions.size());
        j.units.push_back(std::move(u));
        return;
    }
    for (int p : positions) PlanPosition(p, false, base + PositionName(p).substr(OrientationName(o).size()), j);
}

void DicomExplorer::PlanPosition(int p, bool nifti, const std::wstring& base, ExportJob& j)
{
    if (nodes[p].files.size() == 1) { PlanImage(nodes[p].children[0], nifti, base, j); return; }
    ExportUnit u;
    u.stack = StackOf(p);
    if (nifti)
    {
        NiftiUnit(u, { p });
        u.out = base + L".nii";
        u.description = Narrow(base.substr(base.find_last_of(L'\\') + 1));
    }
    else
    {
        u.kind = ExportUnit::Gif;
        u.overTime = true;
        u.out = base + L".gif";
    }
    j.total += static_cast<long>(nodes[p].files.size());
    j.units.push_back(std::move(u));
}

void DicomExplorer::PlanImage(int i, bool nifti, const std::wstring& base, ExportJob& j)
{
    ExportUnit u;
    u.stack = StackOf(i);
    const int frames = u.stack.frames;
    if (nifti)
    {
        NiftiUnit(u, { nodes[i].parent });
        u.out = base + L".nii";
        u.description = Narrow(base.substr(base.find_last_of(L'\\') + 1));
    }
    else
    {
        u.kind = frames > 1 ? ExportUnit::Gif : ExportUnit::Png;
        u.overTime = true;
        u.out = base + (frames > 1 ? L".gif" : L".png");
    }
    j.total += frames;
    j.units.push_back(std::move(u));
}

// A folder to write into (the last one used, the first time Pictures)
std::wstring DicomExplorer::PickFolder(const wchar_t* title, const wchar_t* setting)
{
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return std::wstring();
    DWORD options = 0;
    d->GetOptions(&options);
    d->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(title);
    std::wstring start = ReadText(setting);
    if (start.empty() || !PathIsDirectoryW(start.c_str()))
    {
        PWSTR pictures = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) start = pictures;
        CoTaskMemFree(pictures);
    }
    IShellItem* at = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&at)))) { d->SetFolder(at); at->Release(); }
    std::wstring out;
    if (SUCCEEDED(d->Show(wnd)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item)))
        {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    d->Release();
    if (!out.empty())
        RegSetKeyValueW(HKEY_CURRENT_USER, Settings, setting, REG_SZ, out.c_str(), static_cast<DWORD>((out.size() + 1) * sizeof(wchar_t)));
    return out;
}

void DicomExplorer::Export(int n, bool nifti)
{
    if (!ready || job || n <= 0) return;
    std::wstring folder = PickFolder(nifti ? L"Export to NIfTI: the folder to write into" : L"Export the images: the folder to write into",
                                     L"ExportFolder");
    if (folder.empty()) return;
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
    auto j = std::make_unique<ExportJob>();
    // A study, a patient: a folder of its own
    const Level level = nodes[n].level;
    Plan(n, nifti, level == Level::Patient || level == Level::Study ? folder + L"\\" + FolderName(n) : folder, *j);
    if (j->units.empty())
    {
        SetStatus(L"Nothing to export" + (j->notes.empty() ? std::wstring() : L": " + j->notes[0]));
        return;
    }
    // The window dragged in the preview, when it shows a file of the node
    std::wstring file;
    if (!nifti && preview && preview->DraggedWindow(file, j->center, j->width))
        for (int f : nodes[n].files) if (data->files[f].path == file) { j->dragged = true; break; }
    StartJob(std::move(j));
}

void DicomExplorer::StartJob(std::unique_ptr<ExportJob> j)
{
    job = std::move(j);
    job->window = wnd;
    job->message = WM_EXPORTED;
    job->thread = std::thread(RunExport, job.get());
    SetTimer(wnd, TimerJob, 250, nullptr);
    JobProgress();
}

void DicomExplorer::JobProgress()
{
    if (!job) return;
    wchar_t text[200];
    swprintf_s(text, job->reportPath.empty() ? L"Exporting: %ld of %ld images...   (Esc: stop)" : L"The report: %ld of %ld thumbnails...   (Esc: stop)",
               job->done.load(), job->total);
    SetStatus(text);
}

void DicomExplorer::StopJob()
{
    if (!job) return;
    job->cancel = true;
    if (job->thread.joinable()) job->thread.join();
    job.reset();
}

// Over: what was written, shown in Explorer (the report: opened)
void DicomExplorer::JobDone()
{
    KillTimer(wnd, TimerJob);
    if (!job) return;
    if (job->thread.joinable()) job->thread.join();
    std::unique_ptr<ExportJob> j = std::move(job);
    std::wstring s;
    if (!j->reportPath.empty())
    {
        s = j->written.empty() ? L"The report was not written" : L"Report written: " + j->reportPath;
        if (!j->written.empty()) ShellExecuteW(wnd, L"open", j->reportPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else
    {
        s = std::to_wstring(j->written.size()) + (j->written.size() == 1 ? L" file written" : L" files written");
        if (!j->written.empty())
        {
            const std::wstring& w = j->written[0];
            s += L" in " + w.substr(0, w.find_last_of(L'\\'));
            ShowInExplorer({ w });
        }
    }
    if (j->withoutImage) s += L"   -   " + std::to_wstring(j->withoutImage) + L" without image (reports, PDFs...) left out";
    if (j->cancel) s += L"   -   stopped";
    if (!j->notes.empty())
    {
        s += L"   -   " + std::to_wstring(j->notes.size()) + (j->notes.size() == 1 ? L" note: " : L" notes, the first: ") + j->notes[0];
        std::wstring all;
        for (const auto& note : j->notes) all += note + L"\r\n";
        if (j->notes.size() > 1) CopyText(wnd, all);
    }
    SetStatus(s);
}
