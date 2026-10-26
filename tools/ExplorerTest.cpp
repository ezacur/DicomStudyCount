// Test of the DICOM explorer of bin\DicomShell.dll over a corpus, without its window (built and run by
// tools\Test-DicomExplorer.ps1; DicomShell.cpp is compiled into it, DicomExplorer lets it in).
//   ExplorerTest <out folder> <sample> <log> <folder>...
//       for each folder: the gathering, the tree, the checks, the middle thumbnail of every series, the
//       report, and for a sample of orientations the floating thumbnail and the exports (PNG / GIF /
//       NIfTI), each file written checked and deleted. The folders are only read.
//   ExplorerTest --missing <log> <folder>...
//       the series with instance numbers missing: which ones, their note, the geometry of the series
#include "../src/DicomShell.cpp"
#include <psapi.h>
#include <set>
#pragma comment(lib, "psapi.lib")

struct ExplorerTest
{
    FILE* log = nullptr;
    IWICImagingFactory* factory = nullptr;
    std::map<std::wstring, std::pair<long, std::wstring>> errors;   // what -> (times, an example)

    void Error(const std::wstring& what, const std::wstring& example)
    {
        auto& e = errors[what];
        if (e.first++ == 0) e.second = example;
    }

    void Say(const wchar_t* format, ...)
    {
        va_list a;
        va_start(a, format);
        vfwprintf(log, format, a);
        va_end(a);
        va_start(a, format);
        vwprintf(format, a);
        va_end(a);
        fflush(log);
        fflush(stdout);
    }

    // A file written: readable, its frames and size; a NIfTI: its size from its header
    std::wstring CheckFile(const std::wstring& path, ExportUnit::Kind kind, std::wstring& what)
    {
        if (kind == ExportUnit::Nifti)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return L"NIfTI: not there";
            NiftiHeader h;
            const bool read = fread(&h, sizeof(h), 1, f) == 1;
            _fseeki64(f, 0, SEEK_END);
            const long long size = _ftelli64(f);
            fclose(f);
            if (!read || h.sizeof_hdr != 348 || memcmp(h.magic, "n+1", 4)) return L"NIfTI: bad header";
            long long voxels = 1;
            for (int k = 1; k <= h.dim[0]; k++) voxels *= h.dim[k];
            if (size != 352 + voxels * h.bitpix / 8) return L"NIfTI: size does not match its header";
            for (float v : h.srow_x) if (!std::isfinite(v)) return L"NIfTI: sform not finite";
            wchar_t s[200];
            swprintf_s(s, L"nii %dx%dx%dx%d type %d %s", h.dim[1], h.dim[2], h.dim[3], h.dim[0] > 3 ? h.dim[4] : 1, h.datatype,
                       h.sform_code ? L"placed" : L"NOT placed");
            what = s;
            return L"";
        }
        IWICBitmapDecoder* d = nullptr;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)))
            return L"PNG/GIF: not readable";
        UINT frames = 0, w = 0, h = 0;
        d->GetFrameCount(&frames);
        IWICBitmapFrameDecode* fr = nullptr;
        if (frames && SUCCEEDED(d->GetFrame(0, &fr))) { fr->GetSize(&w, &h); fr->Release(); }
        d->Release();
        if (!frames || !w || !h) return L"PNG/GIF: no frames";
        wchar_t s[120];
        swprintf_s(s, L"%s %ux%u x%u", kind == ExportUnit::Png ? L"png" : L"gif", w, h, frames);
        what = s;
        return L"";
    }

    void Folder(const std::wstring& folder, const std::wstring& out, int sample)
    {
        errors.clear();
        Say(L"\n=== %s\n", folder.c_str());
        const ULONGLONG t0 = GetTickCount64();
        DicomExplorer e;
        e.root = folder;
        e.data = std::make_unique<Gathering>();
        e.data->root = folder;
        Gather(e.data.get());
        const ULONGLONG t1 = GetTickCount64();
        e.Build();
        e.ready = true;
        PROCESS_MEMORY_COUNTERS pm = { sizeof(pm) };
        GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm));
        long patients = 0, studies = 0, series = 0, orientations = 0, troubled = 0;
        std::map<std::wstring, long> flags;
        for (const auto& n : e.nodes)
        {
            if (n.level == Level::Patient) patients++;
            if (n.level == Level::Study) studies++;
            if (n.level == Level::Orientation) orientations++;
            if (n.level == Level::Series) { series++; if (!n.flags.empty()) troubled++; }
            if (n.level == Level::Orientation || n.level == Level::Image || (n.level == Level::Series))
                for (const auto& f : n.problems)
                {
                    std::wstring k = f.substr(0, f.find_first_of(L":0123456789"));
                    if (n.level == Level::Series && f.find(L"files") != std::wstring::npos) k = f.find(L"SOP") != std::wstring::npos ? L"(series) duplicates" : L"(series) truncated";
                    flags[k]++;
                }
        }
        Say(L"read %zu DICOM (+%ld other, %ld DICOMDIR, %ld offline) in %.1f s, tree in %.1f s, %.0f MB peak\n", e.data->files.size(),
            e.data->others, e.data->directories, e.data->offline, (t1 - t0) / 1000.0, (GetTickCount64() - t1) / 1000.0, pm.PeakWorkingSetSize / 1048576.0);
        Say(L"%ld patients, %ld studies, %ld series, %ld orientations; %ld series with problems\n", patients, studies, series, orientations, troubled);
        for (const auto& [k, c] : flags) Say(L"   problem  %-55s %ld\n", k.c_str(), c);
        // Files of three kinds that need more than DCMTK: enhanced multi-frame (their frames apart), float
        // pixels (Parametric Maps), deflated (the whole dataset, or the image frames): how many, and some
        {
            struct Kind { long files = 0, frames = 0; std::vector<std::wstring> examples; };
            std::map<std::wstring, Kind> kinds;
            for (size_t f = 0; f < e.data->files.size(); f++)
            {
                const Gathered& g = e.data->files[f];
                if (g.frame > 0) continue;
                const std::wstring* ts = e.AttributeOf(static_cast<int>(f), 0x00020010);
                const std::wstring* sop = e.AttributeOf(static_cast<int>(f), 0x00080016);
                std::wstring kind;
                if (g.frame == 0) kind = L"enhanced multi-frame: " + (sop ? ObjectType(Narrow(*sop)) : L"?");
                else if (DicomExplorer::Find(g.attributes, 0x7FE00008) || DicomExplorer::Find(g.attributes, 0x7FE00009)) kind = L"float pixels";
                else if (ts && ts->find(L"1.2.840.10008.1.2.1.99") == 0) kind = L"deflated dataset";
                else if (ts && ts->find(L"1.2.840.10008.1.2.8.1") == 0) kind = L"deflated image frames";
                if (kind.empty()) continue;
                Kind& k = kinds[kind];
                k.files++;
                k.frames += g.frame == 0 ? g.frames : 1;
                if (k.examples.size() < 2) k.examples.push_back(g.path);
            }
            for (const auto& [kind, k] : kinds)
            {
                Say(L"   %s: %ld files, %ld frames\n", kind.c_str(), k.files, k.frames);
                for (const auto& x : k.examples) Say(L"            e.g. %s\n", x.c_str());
            }
        }
        // The PatientIDs of several people: what tells them apart (never the names or dates themselves).
        // Only the name apart, with the same birth date, may be one person written two ways
        std::map<std::string, std::vector<int>> byId;
        for (size_t i = 0; i < e.nodes.size(); i++)
            if (e.nodes[i].level == Level::Patient && e.nodes[i].sharing > 1) byId[e.data->files[e.nodes[i].files[0]].patient].push_back(static_cast<int>(i));
        for (const auto& [id, ps] : byId)
        {
            std::vector<Person> people;
            std::wstring hashes;
            for (int p : ps)
            {
                const Gathered& g = e.data->files[e.nodes[p].files[0]];
                people.push_back(PersonOf(g.patientName, g.charset, g.birthDate));
                hashes += L" [" + e.nodes[p].person + L"]";
            }
            bool birth = false, name = false;
            for (size_t a = 0; a < people.size(); a++)
                for (size_t b = a + 1; b < people.size(); b++)
                {
                    if (!people[a].birth.empty() && !people[b].birth.empty() && people[a].birth != people[b].birth) birth = true;
                    if (!people[a].name.empty() && !people[b].name.empty() && people[a].name != people[b].name) name = true;
                }
            // (a name only: how many words each one has, and how many they share)
            std::wstring words;
            if (name && !birth)
                for (size_t a = 0; a + 1 < people.size(); a++)
                {
                    std::vector<std::wstring> both;
                    std::set_intersection(people[a].words.begin(), people[a].words.end(), people[a + 1].words.begin(), people[a + 1].words.end(),
                                          std::back_inserter(both));
                    words += L"  [" + std::to_wstring(people[a].words.size()) + L" and " + std::to_wstring(people[a + 1].words.size()) +
                             L" words, " + std::to_wstring(both.size()) + L" shared]";
                }
            // (a birth date only: how the dates relate - never the dates)
            if (birth && !name)
                for (size_t a = 0; a + 1 < people.size(); a++)
                {
                    const std::string &x = people[a].birth, &y = people[a + 1].birth;
                    const wchar_t* how = x.size() != y.size() ? L"one longer" : x.substr(0, 4) == y.substr(0, 4) ? L"same year" :
                                         x.size() >= 8 && x.substr(4) == y.substr(4) ? L"same month and day, other year" : L"other year";
                    if (x.size() == 8 && y.size() == 8 && (x.substr(4) == "0101" || y.substr(4) == "0101")) how = L"one is January 1st";
                    words += std::wstring(L"  [") + how + L"]";
                }
            Say(L"   shared PatientID '%s': %zu people%s, apart by %s%s\n", Widen(id).c_str(), ps.size(), hashes.c_str(),
                birth && name ? L"name and birth date" : birth ? L"birth date only" : name ? L"NAME ONLY (the same birth date: check)" : L"nothing?",
                words.c_str());
        }
        if (e.data->files.empty()) return;

        // The middle thumbnail of every series (that of the report)
        long thumbs = 0, failed = 0;
        const ULONGLONG t2 = GetTickCount64();
        for (size_t i = 0; i < e.nodes.size(); i++)
        {
            if (e.nodes[i].level != Level::Series) continue;
            Stack s = e.StackOf(e.MainOrientation(static_cast<int>(i)));
            Thumb t;
            try { t = StackThumb(s, 128, 128, true, nullptr); }
            catch (...) { t.error = L"exception"; }
            thumbs++;
            if (t.frames.empty())
            {
                failed++;
                const Gathered& g = e.data->files[e.nodes[i].files[0]];
                Error(L"thumbnail: " + t.error + L"  [" + Compression(Narrow(e.AttributeOf(e.nodes[i].files[0], 0x00020010) ? *e.AttributeOf(e.nodes[i].files[0], 0x00020010) : L"")) +
                      L", " + ObjectType(Narrow(e.AttributeOf(e.nodes[i].files[0], 0x00080016) ? *e.AttributeOf(e.nodes[i].files[0], 0x00080016) : L"")) + L"]", g.path);
            }
        }
        Say(L"middle thumbnails: %ld of %ld series without image, %.1f s\n", failed, thumbs, (GetTickCount64() - t2) / 1000.0);

        // The report
        const ULONGLONG t3 = GetTickCount64();
        const std::wstring report = out + L"\\report.html";
        e.Report(0, report);
        if (e.job)
        {
            e.job->thread.join();
            KillTimer(nullptr, 0);
            const bool ok = !e.job->written.empty();
            e.job.reset();
            WIN32_FILE_ATTRIBUTE_DATA a = {};
            GetFileAttributesExW(report.c_str(), GetFileExInfoStandard, &a);
            Say(L"report: %s, %.1f MB, %.1f s\n", ok ? L"written" : L"NOT WRITTEN", a.nFileSizeLow / 1048576.0, (GetTickCount64() - t3) / 1000.0);
            DeleteFileW(report.c_str());
        }
        else Say(L"report: NOT STARTED\n");

        // A sample of orientations: the floating thumbnail, the images, the NIfTI
        std::vector<int> all;
        for (size_t i = 0; i < e.nodes.size(); i++) if (e.nodes[i].level == Level::Orientation) all.push_back(static_cast<int>(i));
        std::vector<int> chosen;
        for (int k : Spread(0, static_cast<int>(all.size()) - 1, (std::min)(sample, static_cast<int>(all.size())))) chosen.push_back(all[k]);
        long units = 0, written = 0, bad = 0, hover = 0, hoverBad = 0;
        std::map<std::wstring, long> kinds;
        const ULONGLONG t4 = GetTickCount64();
        for (int o : chosen)
        {
            const Gathered& g = e.data->files[e.nodes[o].files[0]];
            const int r = e.nodes[o].parent;
            Thumb h;
            try { h = e.nodes[r].children.size() > 1 ? StackThumb(e.StackOf(o), 160, 128, true, nullptr) : StackThumb(e.StackOf(o), 256, 128, false, nullptr); }
            catch (...) { h.error = L"exception"; }
            hover++;
            if (h.frames.empty()) { hoverBad++; Error(L"hover thumbnail: " + h.error, g.path); }
            for (bool nifti : { false, true })
            {
                ExportJob j;
                e.PlanOrientation(o, nifti, out + L"\\" + e.OrientationName(o), j);
                for (const auto& n : j.notes) Error(L"plan: " + n.substr(n.find(L": ") + 2), g.path);
                for (auto& u : j.units)
                {
                    units++;
                    u.out = FreePath(u.out);
                    std::atomic<bool> cancel{ false };
                    std::atomic<long> done{ 0 };
                    bool wrote = false;
                    std::wstring why;
                    try
                    {
                        why = u.kind == ExportUnit::Nifti ? WriteNifti(u, cancel, done, wrote)
                                                          : WritePictures(factory, u, false, 0, 0, cancel, done, wrote);
                    }
                    catch (...) { why = L"exception"; }
                    if (!why.empty()) Error((u.kind == ExportUnit::Nifti ? L"nifti: " : L"images: ") + why, g.path);
                    if (!wrote) { bad++; DeleteFileW(u.out.c_str()); continue; }
                    std::wstring what;
                    const std::wstring check = CheckFile(u.out, u.kind, what);
                    if (!check.empty()) { bad++; Error(check, u.out); }
                    else
                    {
                        written++;
                        std::wstring kind = what.substr(0, 3);
                        if (what.find(L"NOT placed") != std::wstring::npos) kind += L" (not placed)";
                        kinds[kind]++;
                    }
                    DeleteFileW(u.out.c_str());
                }
            }
        }
        Say(L"sample of %zu orientations (%.1f s): hover thumbnails %ld, %ld without image; exports %ld, %ld written and checked, %ld failed  [",
            chosen.size(), (GetTickCount64() - t4) / 1000.0, hover, hoverBad, units, written, bad);
        for (const auto& [k, c] : kinds) Say(L" %s %ld", k.c_str(), c);
        Say(L" ]\n");
        for (const auto& [what, e2] : errors) Say(L"   ERROR x%ld  %s\n            e.g. %s\n", e2.first, what.c_str(), e2.second.c_str());
    }

    // The series with instance numbers missing: which, and what may explain them
    static std::wstring Ranges(const std::vector<long>& v, size_t most = 12)
    {
        std::wstring s;
        size_t shown = 0;
        for (size_t i = 0; i < v.size() && shown < most; shown++)
        {
            size_t k = i;
            while (k + 1 < v.size() && v[k + 1] == v[k] + 1) k++;
            s += (s.empty() ? L"" : L",") + std::to_wstring(v[i]) + (k > i ? L"-" + std::to_wstring(v[k]) : L"");
            i = k + 1;
        }
        return s;
    }
    void Missing(const std::wstring& folder)
    {
        Say(L"\n=== %s\n", folder.c_str());
        DicomExplorer e;
        e.root = folder;
        e.data = std::make_unique<Gathering>();
        e.data->root = folder;
        Gather(e.data.get());
        e.Build();
        const auto& files = e.data->files;
        auto attr = [&](int f, Uint32 tag) { const std::wstring* v = e.AttributeOf(f, tag); return v ? *v : std::wstring(); };
        for (size_t i = 0; i < e.nodes.size(); i++)
        {
            const GatherNode& r = e.nodes[i];
            if (r.level != Level::Series) continue;
            bool flagged = false;
            for (const auto& p : r.notes) if (p.find(L"Instance numbers missing") == 0) { flagged = true; Say(L"\nNOTE %s", p.c_str()); }
            if (!flagged) continue;
            std::vector<long> have;
            for (int f : r.files) have.push_back(atol(files[f].instance.c_str()));
            std::sort(have.begin(), have.end());
            std::vector<long> missing;
            for (size_t k = 1; k < have.size(); k++) for (long v = have[k - 1] + 1; v < have[k]; v++) missing.push_back(v);
            // The geometry: orientations, positions, images per position, what the checks found in it
            size_t positions = 0;
            std::set<size_t> per;
            std::wstring geometry;
            for (int o : r.children)
            {
                positions += e.nodes[o].children.size();
                for (int p : e.nodes[o].children) per.insert(e.nodes[p].files.size());
                for (const auto& f : e.nodes[o].flags) geometry += L" " + f;
            }
            std::set<std::wstring> types, acquisitions;
            for (int f : r.files) { types.insert(attr(f, 0x00080008)); acquisitions.insert(attr(f, 0x00200012)); }
            std::wstring t;
            for (const auto& x : types) if (t.size() < 120) t += L" {" + x + L"}";
            const Gathered& g = files[r.files[0]];
            Say(L"\nS%s %s  %s  (%zu files, instances %ld..%ld)\n", Widen(g.seriesNumber).c_str(), Widen(g.modality).c_str(),
                Widen(g.seriesDescription, g.charset).c_str(), r.files.size(), have.front(), have.back());
            Say(L"   missing %zu: %s\n", missing.size(), Ranges(missing).c_str());
            std::wstring perText;
            for (size_t k : per) perText += L" " + std::to_wstring(k);
            Say(L"   geometry: %zu orientations, %zu positions, images per position:%s%s%s\n", r.children.size(), positions, perText.c_str(),
                geometry.empty() ? L"" : L", flags:", geometry.c_str());
            Say(L"   ImageType:%s   AcquisitionNumber values: %zu\n", t.c_str(), acquisitions.size());
            Say(L"   e.g. %s\n", g.path.c_str());
        }
    }
};

int wmain(int argc, wchar_t** argv)
{
    if (argc > 3 && !wcscmp(argv[1], L"--missing"))
    {
        ExplorerTest m;
        if (_wfopen_s(&m.log, argv[2], L"w, ccs=UTF-8") || !m.log) { wprintf(L"The log cannot be written: %s\n", argv[2]); return 1; }
        for (int i = 3; i < argc; i++) m.Missing(argv[i]);
        fclose(m.log);
        return 0;
    }
    if (argc < 5) { wprintf(L"ExplorerTest <out folder> <sample> <log> <folder>...\n       ExplorerTest --missing <log> <folder>...\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ExplorerTest t;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t.factory));
    if (_wfopen_s(&t.log, argv[3], L"w, ccs=UTF-8") || !t.log) { wprintf(L"The log cannot be written: %s\n", argv[3]); return 1; }
    SHCreateDirectoryExW(nullptr, argv[1], nullptr);
    for (int i = 4; i < argc; i++)
    {
        try { t.Folder(argv[i], argv[1], _wtoi(argv[2])); }
        catch (...) { t.Say(L"EXCEPTION in %s\n", argv[i]); }
    }
    fclose(t.log);
    return 0;
}
