// The DICOM explorer: the report of a folder (HTML)
#include "Shell.h"

// ----- the report

std::wstring DicomExplorer::Html(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// Where a report goes: a temporary folder of its own (%TEMP%\explorer4dicoms\reports), a name not used
// yet; the browser saves it where it is wanted (Save as). The reports there of more than 7 days go.
std::wstring DicomExplorer::ReportPath(int n) const
{
    wchar_t temp[MAX_PATH + 1] = {};
    if (!GetTempPathW(MAX_PATH + 1, temp)) return std::wstring();
    const std::wstring folder = std::wstring(temp) + L"explorer4dicoms\\reports";
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((folder + L"\\*.html").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const ULONGLONG week = 7ULL * 24 * 3600 * 10000000, t = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        do
        {
            const ULONGLONG w = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && t > w + week) DeleteFileW((folder + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return FreePath(folder + L"\\DICOM report - " + (nodes[n].level == Level::Series ? SeriesName(n) : FolderName(n)) + L".html");
}

// A web page of what there is under a node (a lower one: its series): the patients, their studies and
// a table of their series - a thumbnail, the planes and sizes, the spacings, the Z range, the files,
// what the checks found. Never who the patient is, unless it is not hidden here.
void DicomExplorer::Report(int n, std::wstring path)
{
    if (!ready || job) return;
    if (n < 0) n = 0;
    if (nodes[n].level > Level::Series) n = SeriesOf(n);
    if (path.empty()) path = ReportPath(n);
    if (path.empty()) return;
    auto j = std::make_unique<ExportJob>();
    j->reportPath = path;
    const std::vector<Gathered>& files = data->files;
    auto first = [&](int k, Uint32 tag) {
        const std::wstring* v = AttributeOf(nodes[k].files[0], tag);
        return v ? *v : std::wstring();
    };
    auto mb = [&](int k) {
        long long b = 0;
        for (int f : nodes[k].files) b += files[f].bytes;
        wchar_t s[40];
        swprintf_s(s, L"%.1f", b / 1048576.0);
        return std::wstring(s);
    };
    // Where the files of a node are, from the folder of the report (".\..."): its folder; two or three,
    // each one; more, the folder that holds them all and how many
    auto relative = [&](const std::wstring& dir) {
        if (dir.size() >= root.size() && _wcsnicmp(dir.c_str(), root.c_str(), root.size()) == 0 &&
            (dir.size() == root.size() || dir[root.size()] == L'\\'))
            return L"." + dir.substr(root.size());
        return dir;
    };
    auto where = [&](int k) {
        std::vector<std::wstring> dirs;
        std::unordered_set<std::wstring> seen;
        for (int f : nodes[k].files)
        {
            const std::wstring& p = files[f].path;
            std::wstring d = p.substr(0, p.find_last_of(L'\\'));
            if (seen.insert(d).second) dirs.push_back(std::move(d));
        }
        if (dirs.empty()) return std::wstring();
        if (dirs.size() <= 3)
        {
            std::wstring s;
            for (const auto& d : dirs) s += (s.empty() ? L"" : L"   \u00B7   ") + relative(d);
            return s;
        }
        // (the longest start they share, cut at a backslash)
        std::wstring common = dirs[0];
        for (const auto& d : dirs)
        {
            size_t i = 0;
            while (i < common.size() && i < d.size() && towlower(common[i]) == towlower(d[i])) i++;
            common.resize(i);
        }
        const bool whole = std::all_of(dirs.begin(), dirs.end(), [&](const std::wstring& d) { return d.size() == common.size() || d[common.size()] == L'\\'; });
        if (!whole) common.resize(common.find_last_of(L'\\') == std::wstring::npos ? 0 : common.find_last_of(L'\\'));
        return (common.empty() ? std::wstring() : relative(common) + L"  ") + L"(" + std::to_wstring(dirs.size()) + L" folders)";
    };
    // The patients, studies and series under the node (or above it, for a series)
    std::vector<int> series;
    std::function<void(int)> walk = [&](int k) {
        if (nodes[k].level == Level::Series) { series.push_back(k); return; }
        for (int c : nodes[k].children) walk(c);
    };
    walk(n);
    size_t troubled = 0, studies = 0, images = 0;
    std::vector<int> patients, studyList;
    for (int r : series)
    {
        const int s = nodes[r].parent, p = nodes[s].parent;
        if (std::find(studyList.begin(), studyList.end(), s) == studyList.end()) studyList.push_back(s);
        if (std::find(patients.begin(), patients.end(), p) == patients.end()) patients.push_back(p);
        if (!nodes[r].flags.empty()) troubled++;
        images += FilesOf(r);
    }
    studies = studyList.size();
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t when[64];
    swprintf_s(when, L"%04d-%02d-%02d %02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    const std::wstring scope = n == 0 ? root : root + L"   \u203A   " + nodes[n].text;

    std::wstring h;
    h += L"<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
         L"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>DICOM report</title>\n<style>\n"
         L":root{--bg:#ffffff;--fg:#1c1c1e;--muted:#6b6b72;--line:#e2e2e7;--card:#f6f6f8;--warn:#a84f00;--warnbg:#fff4e6;--link:#0a58ca;"
         L"--patient:#c00000;--study:#007a00;--series:#0046dc}\n"
         L"@media (prefers-color-scheme:dark){:root{--bg:#141416;--fg:#ececf0;--muted:#a1a1aa;--line:#2c2c31;--card:#1d1d21;"
         L"--warn:#f5a524;--warnbg:#2b2112;--link:#7ab0ff;--patient:#ff7b7b;--study:#6ad46a;--series:#7aa7ff}}\n"
         L"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 \"Segoe UI\",system-ui,sans-serif}\n"
         L"main{max-width:1320px;margin:0 auto;padding:28px 16px 56px}h1{font-size:24px;margin:0 0 4px}\n"
         L".meta{color:var(--muted);overflow-wrap:anywhere}.totals{display:flex;flex-wrap:wrap;gap:10px;margin:18px 0 8px}\n"
         L".totals div{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 14px;min-width:110px}\n"
         L".totals b{display:block;font-size:22px;font-variant-numeric:tabular-nums}\n"
         L"h2{font-size:19px;margin:34px 0 4px;color:var(--patient)}h3{font-size:16px;margin:24px 0 2px;color:var(--study)}\n"
         L".wrap{overflow-x:auto;margin-top:8px;border:1px solid var(--line);border-radius:10px}\n"
         L".study{border-left:6px solid var(--c);padding:2px 0 6px 14px;margin-top:24px}.study h3{margin-top:0}"
         L".study .wrap{border:5px solid var(--c);outline:1px solid var(--line)}\n"
         L".sw{display:inline-block;width:14px;height:14px;border-radius:3px;background:var(--c);outline:1px solid var(--line);"
         L"vertical-align:-2px;margin-right:8px}\n"
         L".dot{display:inline-block;width:10px;height:10px;border-radius:50%;outline:1px solid var(--line);margin-right:6px}\n"
         L".where{font-family:Consolas,\"Cascadia Mono\",monospace;font-size:12px;color:var(--muted);overflow-wrap:anywhere}"
         L"details.patient>summary .where{margin:2px 0 0 18px}\n"
         L"summary{cursor:pointer}summary h2,summary h3{display:inline;margin:0}details.patient{margin-top:30px}"
         L"details.study>summary .meta{margin:2px 0 0 18px}details.study>summary .sub{margin:6px 0 0 18px}details.study:not([open]){padding-bottom:2px}\n"
         L".tools{margin-top:20px}.tools button{font:inherit;padding:4px 12px;margin-right:6px;border:1px solid var(--line);"
         L"border-radius:6px;background:var(--card);color:var(--fg);cursor:pointer}\n"
         L"table{border-collapse:collapse;width:100%;min-width:980px}th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--line);vertical-align:top}\n"
         L"tr:last-child td{border-bottom:0}th{color:var(--muted);font-weight:600;font-size:12px;background:var(--card)}\n"
         L"td.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}td.series{color:var(--series);font-weight:600}\n"
         L"img.t{display:block;width:auto;height:auto;max-width:128px;max-height:128px;border-radius:4px;background:#000}\n"
         L"tr.warn td{background:var(--warnbg)}.warn{color:var(--warn)}ul{margin:6px 0 0;padding-left:20px}li{margin:2px 0}\n"
         L"a{color:var(--link)}li.note{color:var(--muted)}.problems{background:var(--warnbg);border:1px solid var(--line);border-radius:10px;padding:12px 16px;margin-top:18px}\n"
         L".problems h2{margin:0;color:var(--warn);font-size:17px}small{color:var(--muted)}.notes{border:1px solid var(--line);border-radius:10px;padding:10px 16px;margin-top:12px;color:var(--muted)}.notes summary{cursor:pointer;font-weight:600}\n"
         L"footer{margin-top:40px;color:var(--muted);font-size:12px}\n</style>\n</head>\n<body>\n<main>\n";
    h += L"<h1>DICOM report</h1>\n<div class=\"meta\">" + Html(scope) + L"<br>" + when + L"</div>\n";
    auto total = [&](size_t v, const wchar_t* one, const wchar_t* many) {
        h += L"<div><b>" + std::to_wstring(v) + L"</b>" + (v == 1 ? one : many) + L"</div>";
    };
    h += L"<div class=\"totals\">";
    total(images, L"DICOM file", L"DICOM files");
    total(patients.size(), L"patient", L"patients");
    total(studies, L"study", L"studies");
    total(series.size(), L"series", L"series");
    if (troubled) h += L"<div class=\"warn\"><b>" + std::to_wstring(troubled) + L"</b>series with problems</div>";
    h += L"</div>\n";
    if (n == 0)
    {
        std::wstring also;
        if (data->others) also += std::to_wstring(data->others) + L" other files. ";
        if (data->directories) also += std::to_wstring(data->directories) + L" DICOMDIR. ";
        if (data->offline) also += std::to_wstring(data->offline) + L" files not downloaded (not read). ";
        if (!also.empty()) h += L"<div class=\"meta\">" + Html(also) + L"</div>\n";
    }
    // The color of a study, the one of its folders and icons: never under text, only as marks
    auto color = [&](int study) {
        const std::string& uid = files[nodes[study].files[0]].study;
        wchar_t c[16];
        swprintf_s(c, L"#%06X", Palette[uid.empty() ? NoStudyColor : ColorIndex(uid)]);
        return std::wstring(c);
    };
    auto dot = [&](int r) { return L"<span class=\"dot\" style=\"background:" + color(nodes[r].parent) + L"\"></span>"; };
    // (a PatientID of several people, too)
    std::vector<int> shared;
    for (int p : patients) if (nodes[p].sharing > 1) shared.push_back(p);
    if (troubled || !shared.empty())
    {
        h += L"<section class=\"problems\"><h2>\u26A0 Problems</h2><ul>\n";
        for (int p : shared)
            h += L"<li>" + Html(Plain(p)) + L": the same Patient ID, " + std::to_wstring(nodes[p].sharing) +
                 L" people (other names or birth dates): shown apart</li>\n";
        for (int r : series)
        {
            if (nodes[r].flags.empty()) continue;
            std::wstring f;
            for (size_t k = 0; k < nodes[r].flags.size(); k++) f += (k ? L", " : L"") + nodes[r].flags[k];
            h += L"<li>" + dot(r) + L"<a href=\"#s" + std::to_wstring(r) + L"\">" + Html(Plain(r)) + L"</a>: " + Html(f) + L"</li>\n";
        }
        h += L"</ul></section>\n";
    }
    // The notes (instance numbers missing, and how): folded, under the problems
    size_t noted = 0;
    for (int r : series) if (!nodes[r].notes.empty()) noted++;
    if (noted)
    {
        h += L"<details class=\"notes\"><summary>Notes: " + std::to_wstring(noted) + L" series" +
             L" with instance numbers missing (not a problem by itself: the positions say whether a slice is missing)</summary><ul>\n";
        for (int r : series)
            for (const auto& note : nodes[r].notes)
                h += L"<li>" + dot(r) + L"<a href=\"#s" + std::to_wstring(r) + L"\">" + Html(Plain(r)) + L"</a>: " + Html(note) + L"</li>\n";
        h += L"</ul></details>\n";
    }
    h += L"<div class=\"tools\"><button type=\"button\" onclick=\"fold(true)\">Expand all</button>"
         L"<button type=\"button\" onclick=\"fold(false)\">Collapse all</button></div>\n";
    for (int p : patients)
    {
        const Gathered& g = files[nodes[p].files[0]];
        std::wstring who = g.patient.empty() ? L"Patient (no ID)" : L"Patient " + Widen(g.patient, g.charset);
        if (!nodes[p].person.empty()) who += L" \x00B7 [" + nodes[p].person + L"]";
        if (!hideIdentity)
        {
            for (Uint32 tag : { 0x00100010u, 0x00100040u, 0x00100030u })
            {
                std::wstring v = first(p, tag);
                if (!v.empty()) who += L"   \u00B7   " + v;
            }
        }
        // A patient of one study folds as one: the patient and the study in the same title
        const bool alone = std::count_if(studyList.begin(), studyList.end(), [&](int s) { return nodes[s].parent == p; }) == 1;
        if (!alone)
            h += L"<details class=\"patient\" open><summary><h2>" + Html(who) + L"</h2><div class=\"meta where\">" + Html(where(p)) +
                 L"</div></summary>\n";
        for (int s : studyList)
        {
            if (nodes[s].parent != p) continue;
            const Gathered& sg = files[nodes[s].files[0]];
            std::wstring title = DateTimeText(sg.studyDate, sg.studyTime);
            const std::wstring description = Widen(sg.studyDescription, sg.charset);
            if (!description.empty()) title += (title.empty() ? L"" : L"   \u00B7   ") + description;
            if (title.empty()) title = L"Study";
            // (folded: its title, its color and its counts)
            h += std::wstring(alone ? L"<details class=\"patient study\"" : L"<details class=\"study\"") + L" style=\"--c:" + color(s) + L"\"" +
                 (studies > 3 ? L"" : L" open") + L"><summary>" + (alone ? L"<h2>" + Html(who) + L"</h2><div class=\"sub\">" : L"") +
                 L"<h3><span class=\"sw\"></span>" + Html(title) + L"</h3>" + (alone ? L"</div>" : L"") + L"\n<div class=\"meta\">";
            std::wstring line;
            auto add = [&](const std::wstring& name, const std::wstring& v) { if (!v.empty()) line += (line.empty() ? L"" : L"   \u00B7   ") + name + v; };
            if (!sg.study.empty()) add(L"", L"[" + Hash6(sg.study) + L"]");
            add(L"Accession ", first(s, 0x00080050));
            add(L"Study ID ", first(s, 0x00200010));
            add(L"", std::to_wstring(nodes[s].children.size()) + L" series");
            add(L"", HowMany(s));
            add(L"", mb(s) + L" MB");
            line = Html(line) + L"<br><span class=\"where\">" + Html(where(s)) + L"</span>";
            h += line + L"</div></summary>\n<div class=\"wrap\"><table>\n<thead><tr><th></th><th>Series</th><th>Modality</th><th>Description</th>"
                 L"<th>Plane, columns x rows x positions (x times)</th><th>Pixel, thickness (mm)</th><th>Z (mm)</th><th class=\"n\">Files</th>"
                 L"<th class=\"n\">MB</th><th>Problems, notes</th></tr></thead>\n<tbody>\n";
            for (int r : series)
            {
                if (nodes[r].parent != s) continue;
                const Gathered& rg = files[nodes[r].files[0]];
                const size_t thumb = j->stacks.size();
                j->stacks.push_back(StackOf(MainOrientation(r)));
                std::wstring planes, spacing, zs;
                for (int o : nodes[r].children)
                {
                    const std::wstring br = planes.empty() ? L"" : L"<br>";
                    planes += br + Html(Size(o));
                    std::wstring sp = first(o, 0x00280030);
                    for (auto& c : sp) if (c == L'\\') c = L'x';
                    const std::wstring th = first(o, 0x00180050);
                    spacing += br + Html(sp + (th.empty() ? L"" : L",  " + th));
                    std::vector<double> z;
                    for (int c : nodes[o].children) if (files[nodes[c].files[0]].hasZ) z.push_back(files[nodes[c].files[0]].z);
                    wchar_t b[120] = L"";
                    if (z.size() == nodes[o].children.size() && z.size() > 1)
                    {
                        std::vector<double> steps;
                        for (size_t k = 1; k < z.size(); k++) steps.push_back(z[k] - z[k - 1]);
                        std::sort(steps.begin(), steps.end());
                        swprintf_s(b, L"%.1f to %.1f, step %.3g", z.front(), z.back(), steps[steps.size() / 2]);
                    }
                    else if (z.size() == 1) swprintf_s(b, L"%.1f", z[0]);
                    zs += br + b;
                }
                std::wstring problems;
                for (const auto& pr : ProblemsUnder(r, 12)) problems += L"<li>" + Html(pr) + L"</li>";
                for (const auto& note : nodes[r].notes) problems += L"<li class=\"note\">" + Html(note) + L"</li>";
                h += L"<tr id=\"s" + std::to_wstring(r) + L"\"" + (nodes[r].flags.empty() ? L"" : L" class=\"warn\"") + L">";
                h += L"<td><img class=\"t\" alt=\"\" src=\"{{thumb " + std::to_wstring(thumb) + L"}}\"></td>";
                h += L"<td class=\"series\">" + Html(Widen(rg.seriesNumber)) + L"</td><td>" + Html(Widen(rg.modality)) + L"</td><td>" +
                     Html(Widen(rg.seriesDescription, rg.charset)) + L"<br><small>" + Html(DateTimeText(rg.seriesDate, rg.seriesTime)) + L"</small><br><small class=\"where\">" + Html(where(r)) + L"</small></td>";
                h += L"<td>" + planes + L"</td><td>" + spacing + L"</td><td>" + zs + L"</td>";
                h += L"<td class=\"n\">" + std::to_wstring(FilesOf(r)) + L"</td><td class=\"n\">" + mb(r) + L"</td>";
                h += L"<td class=\"warn\">" + (problems.empty() ? L"" : L"<ul>" + problems + L"</ul>") + L"</td></tr>\n";
            }
            h += L"</tbody></table></div></details>\n";
        }
        if (!alone) h += L"</details>\n";
    }
    // A link to a series opens what it is folded in
    h += L"<script>\nfunction reveal(id){var e=document.getElementById(id);for(var p=e;p;p=p.parentElement)if(p.tagName==='DETAILS')p.open=true;"
         L"if(e)e.scrollIntoView();}\ndocument.addEventListener('click',function(ev){var a=ev.target.closest('a[href^=\"#s\"]');"
         L"if(a){ev.preventDefault();reveal(a.getAttribute('href').slice(1));history.replaceState(null,'',a.getAttribute('href'));}});\n"
         L"if(location.hash)reveal(location.hash.slice(1));\n"
         L"function fold(open){document.querySelectorAll('details.patient,details.study').forEach(function(d){d.open=open;});}\n</script>\n";
    h += L"<footer>Written by the DICOM explorer of explorer4dicoms. The files were only read.</footer>\n</main>\n</body>\n</html>\n";
    j->report = std::move(h);
    j->total = static_cast<long>(j->stacks.size());
    StartJob(std::move(j));
}

// (not into the images: a series can have thousands)
void DicomExplorer::ExpandAll(int n, bool open)
{
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    std::function<void(int)> walk = [&](int i) {
        if (nodes[i].shown.empty() || !nodes[i].item) return;
        if (open && std::all_of(nodes[i].shown.begin(), nodes[i].shown.end(), [&](int c) { return nodes[c].level == Level::Image; }) && i != n)
            return;
        TreeView_Expand(tree, nodes[i].item, open ? TVE_EXPAND : TVE_COLLAPSE);
        for (int c : nodes[i].shown) walk(c);
    };
    walk(n);
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    if (nodes[n].item) TreeView_EnsureVisible(tree, nodes[n].item);
}
