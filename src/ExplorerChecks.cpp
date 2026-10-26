// The DICOM explorer: the checks, what may be wrong in the series
#include "Shell.h"

// ----- the checks: what may be wrong in the series

void DicomExplorer::Problem(int n, const std::wstring& flag, const std::wstring& text)
{
    nodes[n].problems.push_back(text);
    if (std::find(nodes[n].flags.begin(), nodes[n].flags.end(), flag) == nodes[n].flags.end()) nodes[n].flags.push_back(flag);
}

// The value of an attribute in a file, if it has it
const std::wstring* DicomExplorer::AttributeOf(int f, Uint32 tag) const
{
    const auto* a = Find(data->files[f].attributes, tag);
    return a ? &data->pool.Text(a->second) : nullptr;
}

// The groups of the overlay planes of a file (6000, 6002...: those with an OverlayRows, 60xx,0010)
std::vector<Uint32> DicomExplorer::OverlayGroupsOf(int f) const
{
    std::vector<Uint32> groups;
    for (Uint32 group = 0x6000; group <= 0x601E; group += 2)
        if (AttributeOf(f, (group << 16) | 0x0010)) groups.push_back(group);
    return groups;
}

// Each orientation: gaps in its positions, spacings that are not regular, positions off the line of the
// normal, positions with another number of images, images without position, pixel spacings that vary.
// Each series: instance numbers missing. Each file: the same SOP instance UID as another one, shorter
// than its pixels. The series show what was found under them, the studies and patients how many series.
void DicomExplorer::Check()
{
    const std::vector<Gathered>& files = data->files;
    wchar_t b[400];
    auto mm = [](double v) { wchar_t s[40]; swprintf_s(s, L"%.4g", v); return std::wstring(s); };
    auto fileName = [&](int f) { return files[f].path.substr(files[f].path.find_last_of(L'\\') + 1); };

    // The same SOP instance UID in more than one file (copies)
    std::unordered_map<Uint32, std::vector<int>> uids;
    for (size_t f = 0; f < files.size(); f++)
    {
        if (files[f].frame > 0) continue;       // (the frames of a file share its UID)
        if (const auto* a = Find(files[f].attributes, 0x00080018)) uids[a->second].push_back(static_cast<int>(f));
    }
    for (const auto& [uid, same] : uids)
    {
        if (same.size() < 2) continue;
        for (int f : same)
        {
            std::wstring others;
            for (int g : same) if (g != f && others.size() < 200) others += (others.empty() ? L"" : L", ") + files[g].path;
            Problem(fileState[f].node, L"duplicate", L"The same SOP instance UID as " + others);
        }
    }

    for (size_t i = 0; i < nodes.size(); i++)
    {
        const int n = static_cast<int>(i);
        const GatherNode& node = nodes[n];
        if (node.level == Level::Image)
        {
            // Shorter than its pixels (uncompressed only: the size of compressed ones is not known)
            const int f = node.files[0];
            if (files[f].frame > 0) continue;              // (once a file: its first frame)
            const std::wstring* ts = AttributeOf(f, 0x00020010);
            std::wstring syntax = ts ? *ts : L"";
            while (!syntax.empty() && (syntax.back() == L' ' || syntax.back() == 0)) syntax.pop_back();
            if (syntax != L"1.2.840.10008.1.2" && syntax != L"1.2.840.10008.1.2.1" && syntax != L"1.2.840.10008.1.2.2") continue;
            const std::wstring* bits = AttributeOf(f, 0x00280100);
            const std::wstring* samples = AttributeOf(f, 0x00280002);
            const long long need = static_cast<long long>(files[f].rows) * files[f].columns * files[f].frames *
                                   (samples ? (std::max)(1, _wtoi(samples->c_str())) : 1) * (bits ? _wtoi(bits->c_str()) : 0) / 8;
            if (need > 0 && files[f].bytes < need)
            {
                swprintf_s(b, L"The file is shorter than its pixels: %lld bytes, and its pixels alone need %lld", files[f].bytes, need);
                Problem(n, L"truncated", b);
            }
            continue;
        }
        if (node.level == Level::Series)
        {
            // Instance numbers missing (when there are few of them, and none repeated): a note, not a
            // problem. In a corpus of 700 000 files almost all of them were the numbering of the scanner
            // (magnitude and phase interleaved, the same phases left out of every slice) or images the
            // operator did not keep; images really missing leave a hole in the positions, checked apart.
            std::vector<long> numbers;
            for (int f : node.files)
            {
                if (files[f].frame > 0) continue;          // (the frames of a file share its number)
                const std::string& s = files[f].instance;
                if (s.empty()) { numbers.clear(); break; }
                numbers.push_back(atol(s.c_str()));
            }
            std::sort(numbers.begin(), numbers.end());
            if (numbers.size() >= 3 && std::adjacent_find(numbers.begin(), numbers.end()) == numbers.end())
            {
                const long long missing = static_cast<long long>(numbers.back()) - numbers.front() + 1 - static_cast<long long>(numbers.size());
                if (missing > 0 && missing <= static_cast<long long>(numbers.size()))
                {
                    std::wstring which;
                    int listed = 0;
                    for (size_t k = 1; k < numbers.size() && listed < 6; k++)
                        for (long v = numbers[k - 1] + 1; v < numbers[k] && listed < 6; v++, listed++)
                            which += (which.empty() ? L"" : L", ") + std::to_wstring(v);
                    if (missing > listed) which += L"...";
                    // Regular: most runs of the same length, most of them the same distance from the one
                    // before (a few may be off: the same phase dropped from every slice but one)
                    std::vector<std::pair<long, long>> runs;            // (first missing, how many)
                    for (size_t k = 1; k < numbers.size(); k++)
                        if (numbers[k] > numbers[k - 1] + 1) runs.push_back({ numbers[k - 1] + 1, numbers[k] - numbers[k - 1] - 1 });
                    auto mostCommon = [](const std::vector<long>& v, size_t& times) {
                        std::map<long, size_t> c;
                        for (long x : v) c[x]++;
                        long best = 0;
                        times = 0;
                        for (const auto& [x, k] : c) if (k > times) { times = k; best = x; }
                        return best;
                    };
                    std::vector<long> lengths, steps;
                    for (size_t k = 0; k < runs.size(); k++)
                    {
                        lengths.push_back(runs[k].second);
                        if (k) steps.push_back(runs[k].first - runs[k - 1].first);
                    }
                    size_t sameLength = 0, sameStep = 0;
                    const long length = mostCommon(lengths, sameLength), step = mostCommon(steps, sameStep);
                    const bool regular = runs.size() >= 3 && sameLength * 3 >= runs.size() * 2 && sameStep * 3 >= steps.size() * 2;
                    std::wstring why = L"few and scattered: images not kept by the operator, or lost - the positions say whether a slice is missing";
                    if (regular)
                    {
                        wchar_t r[200];
                        swprintf_s(r, L"%s, %ld of every %ld: the numbering of the scanner, or images left out on purpose",
                                   sameStep == steps.size() && sameLength == runs.size() ? L"regular" : L"mostly regular", length, step);
                        why = r;
                    }
                    else if (runs.size() == 1 && runs[0].second > 1) why = L"one block: images not kept, or lost - the positions say whether a slice is missing";
                    else if (runs.size() == 2 && runs[0].second == runs[1].second && runs[0].second > 1)
                        why = L"two blocks of " + std::to_wstring(runs[0].second) + L": images not kept, or lost - the positions say whether a slice is missing";
                    swprintf_s(b, L"Instance numbers missing: %lld between %ld and %ld (%s). %s", missing, numbers.front(), numbers.back(),
                               which.c_str(), why.c_str());
                    nodes[n].notes.push_back(b);
                }
            }
            continue;
        }
        if (node.level != Level::Orientation) continue;
        const std::vector<int>& positions = node.children;
        const Gathered& g0 = files[node.files[0]];
        // Images without position, among others that have it
        size_t placed = 0;
        for (int f : node.files) if (files[f].placed) placed++;
        if (placed > 0 && placed < node.files.size())
            Problem(n, L"no position", std::to_wstring(node.files.size() - placed) + L" of its images have no position (ImagePositionPatient)");
        // Pixel spacing
        std::vector<std::wstring> spacings;
        for (int f : node.files)
        {
            const std::wstring* s = AttributeOf(f, 0x00280030);
            if (s && std::find(spacings.begin(), spacings.end(), *s) == spacings.end()) spacings.push_back(*s);
        }
        if (spacings.size() > 1)
            Problem(n, L"pixel spacing varies", L"Pixel spacing varies: " + spacings[0] + L"  |  " + spacings[1] + (spacings.size() > 2 ? L"  |  ..." : L""));
        if (positions.size() < 2) continue;
        // Images per position
        std::map<size_t, size_t> counts;
        for (int p : positions) counts[nodes[p].files.size()]++;
        if (counts.size() > 1)
        {
            size_t most = 0, usual = 0;
            for (const auto& [k, c] : counts) if (c > most) { most = c; usual = k; }
            std::wstring which;
            size_t differ = 0;
            for (int p : positions)
            {
                const size_t k = nodes[p].files.size();
                if (k == usual) continue;
                differ++;
                if (differ <= 3)
                {
                    const Gathered& g = files[nodes[p].files[0]];
                    swprintf_s(b, L"%s%s has %zu", which.empty() ? L"" : L", ", g.hasZ ? (L"Z " + mm(g.z)).c_str() : L"one", k);
                    which += b;
                }
                Problem(p, std::to_wstring(k) + L" images (most have " + std::to_wstring(usual) + L")",
                        L"This position has " + std::to_wstring(k) + L" images; most positions of its orientation have " + std::to_wstring(usual));
            }
            swprintf_s(b, L"Images per position: most have %zu; %s%s (%zu positions differ)", usual, which.c_str(), differ > 3 ? L"..." : L"", differ);
            Problem(n, L"images per position differ", b);
        }
        // The positions along the normal: gaps, irregular spacing; off its line
        std::vector<const Gathered*> first;
        for (int p : positions) first.push_back(&files[nodes[p].files[0]]);
        if (std::all_of(first.begin(), first.end(), [](const Gathered* g) { return g->placed && g->oriented && g->hasZ; }))
        {
            const double* o = g0.orientation;
            double shift = 0;
            for (const Gathered* g : first)
            {
                double d[3];
                for (int r = 0; r < 3; r++) d[r] = g->position[r] - first[0]->position[r];
                const double u = d[0] * o[0] + d[1] * o[1] + d[2] * o[2], v = d[0] * o[3] + d[1] * o[4] + d[2] * o[5];
                shift = (std::max)(shift, sqrt(u * u + v * v));
            }
            if (shift > 0.05)
                Problem(n, L"shifted", L"The positions are not on one line along the normal: up to " + mm(shift) +
                                       L" mm off in the plane of the images (a gantry tilt, or stacks put together)");
            if (positions.size() < 3) continue;
            std::vector<double> steps;
            for (size_t k = 1; k < first.size(); k++) steps.push_back(first[k]->z - first[k - 1]->z);
            std::vector<double> sorted = steps;
            std::sort(sorted.begin(), sorted.end());
            const double usual = sorted[sorted.size() / 2];
            if (!(usual > 1e-3)) continue;
            int gaps = 0;
            long long lost = 0;
            double least = usual, largest = usual;
            std::wstring where;
            for (size_t k = 0; k < steps.size(); k++)
            {
                if (steps[k] > 1.5 * usual)
                {
                    if (gaps++ == 0) where = L"from Z " + mm(first[k]->z) + L" to " + mm(first[k + 1]->z) + L" mm";
                    lost += std::llround(steps[k] / usual) - 1;
                }
                else { least = (std::min)(least, steps[k]); largest = (std::max)(largest, steps[k]); }
            }
            if (gaps)
            {
                swprintf_s(b, L"Gap: about %lld position%s missing %s (the usual spacing is %s mm)%s", lost, lost == 1 ? L"" : L"s",
                           where.c_str(), mm(usual).c_str(), gaps > 1 ? (L", and " + std::to_wstring(gaps - 1) + L" more gaps").c_str() : L"");
                Problem(n, L"gap", b);
            }
            if (largest - least > 0.1 * usual)
                Problem(n, L"irregular spacing", L"The spacing of its positions is not regular: from " + mm(least) + L" to " + mm(largest) +
                                                 L" mm (usually " + mm(usual) + L" mm)");
        }
    }

    // Up the tree: a series, what was found under it; a study and a patient, how many series
    for (int i = static_cast<int>(nodes.size()) - 1; i > 0; i--)
    {
        GatherNode& node = nodes[i];
        if (node.level == Level::Series)
        {
            std::map<std::wstring, size_t> images;
            std::function<void(int)> walk = [&](int k) {
                for (int c : nodes[k].children)
                {
                    if (nodes[c].level == Level::Orientation)
                        for (const auto& f : nodes[c].flags)
                            if (std::find(node.flags.begin(), node.flags.end(), f) == node.flags.end()) node.flags.push_back(f);
                    if (nodes[c].level == Level::Image) for (const auto& f : nodes[c].flags) images[f]++;
                    walk(c);
                }
            };
            walk(i);
            for (const auto& [flag, k] : images)
            {
                node.flags.push_back(std::to_wstring(k) + L" " + flag + (flag == L"duplicate" && k != 1 ? L"s" : L""));
                node.problems.push_back(std::to_wstring(k) + (k == 1 ? L" file " : L" files ") +
                                        (flag == L"duplicate" ? (k == 1 ? L"has the same SOP instance UID as another file (a copy)"
                                                                        : L"have the same SOP instance UID as other files (copies)")
                                                              : (k == 1 ? L"is shorter than its pixels" : L"are shorter than their pixels")));
            }
        }
        else if ((node.level == Level::Study || node.level == Level::Patient))
        {
            size_t k = 0;
            std::function<void(int)> walk = [&](int c) {
                if (nodes[c].level == Level::Series) { if (!nodes[c].flags.empty()) k++; return; }
                for (int d : nodes[c].children) walk(d);
            };
            walk(i);
            // (first who it is: the problems of its series after)
            if (node.level == Level::Patient && node.sharing > 1)
                Problem(i, L"the same ID, " + std::to_wstring(node.sharing) + L" people",
                        L"Its PatientID is that of " + std::to_wstring(node.sharing) + L" people (other names or birth dates in the files): shown apart. " +
                        L"An export that puts the same placeholder in every PatientID (9999), or files without one");
            if (k) node.flags.push_back(std::to_wstring(k) + L" series with problems");
        }
    }
}

// The problems of a node and of those under it, those under it with the line of their node (`most` at
// most); not those of each position and image (a series counts those of its images)
std::vector<std::wstring> DicomExplorer::ProblemsUnder(int n, size_t most) const
{
    std::vector<std::wstring> out;
    size_t more = 0;
    std::function<void(int)> walk = [&](int k) {
        std::wstring where;
        const bool alone = nodes[k].level == Level::Orientation && nodes[nodes[k].parent].children.size() == 1;
        if (k != n && !alone)
        {
            where = nodes[k].text;
            const size_t mark = where.find(L"   \u26A0");
            if (mark != std::wstring::npos) where.resize(mark);
            where += L":   ";
        }
        for (const auto& p : nodes[k].problems)
        {
            if (out.size() < most) out.push_back(where + p);
            else more++;
        }
        for (int c : nodes[k].children)
            if (nodes[c].level != Level::Position && nodes[c].level != Level::Image) walk(c);
    };
    walk(n);
    if (more) out.push_back(L"... and " + std::to_wstring(more) + L" more");
    return out;
}

// A series: its orientations (the same ImageOrientationPatient, rows and columns), their positions
// (the same ImagePositionPatient, in the order of their Z level) and their images
void DicomExplorer::AddSeries(int study, const std::vector<int>& rg)
{
    const std::vector<Gathered>& files = data->files;
    const int r = Add(Level::Series, study, rg);
    std::vector<std::vector<int>> orientations;
    for (int f : rg)
    {
        const Gathered& g = files[f];
        auto same = [&](const std::vector<int>& o) {
            const Gathered& h = files[o[0]];
            if (g.oriented != h.oriented || g.rows != h.rows || g.columns != h.columns) return false;
            if (fileState[f].derived != fileState[o[0]].derived || fileState[f].frame != fileState[o[0]].frame) return false;
            for (int k = 0; k < 6 && g.oriented; k++) if (std::fabs(g.orientation[k] - h.orientation[k]) > 1e-4) return false;
            return true;
        };
        auto it = std::find_if(orientations.begin(), orientations.end(), same);
        if (it == orientations.end()) orientations.push_back({ f });
        else it->push_back(f);
    }
    for (auto& og : orientations)
    {
        const int o = Add(Level::Orientation, r, og);
        // Placed ones by their Z level (the position along the normal), those without a position last
        std::vector<int> sorted = og;
        std::stable_sort(sorted.begin(), sorted.end(), [&](int a, int b) {
            const Gathered &x = files[a], &y = files[b];
            if (x.placed != y.placed) return x.placed;
            if (x.hasZ && y.hasZ && x.z != y.z) return x.z < y.z;
            return false;
        });
        std::vector<std::vector<int>> positions;
        auto same = [&](const Gathered& g, const Gathered& h) {
            if (g.placed != h.placed) return false;
            for (int k = 0; k < 3 && g.placed; k++) if (std::fabs(g.position[k] - h.position[k]) >= 1e-3) return false;
            return true;
        };
        for (int f : sorted)
        {
            const Gathered& g = files[f];
            bool joined = false;
            if (!positions.empty() && same(g, files[positions.back()[0]]))
            {
                positions.back().push_back(f);
                joined = true;
            }
            // (a position seen before, not the last one: without Z levels they are not sorted)
            for (size_t k = 0; !joined && g.placed && k < positions.size(); k++)
                if (same(g, files[positions[k][0]])) { positions[k].push_back(f); joined = true; }
            if (!joined) positions.push_back({ f });
        }
        for (auto& pg : positions)
        {
            // The images of a position in their order (instance number, trigger time...)
            std::sort(pg.begin(), pg.end(), [&](int a, int b) { return fileState[a].rank < fileState[b].rank; });
            const int z = Add(Level::Position, o, pg);
            for (int f : pg) fileState[f].node = Add(Level::Image, z, { f });
        }
    }
}
