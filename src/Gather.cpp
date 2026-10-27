// The gathering of the DICOM files of a folder for the explorer (DICOMgather.m): their attributes, the people
#include "Shell.h"

// ---------------------------------------------------------------------------------------------
// DICOM explorer (rundll32 DicomShell.dll,Explore <folder>; "DICOM explorer" in the DICOM menu of a
// folder): the DICOM files under a folder as the tree of DCMexplorer.m / DICOMgather.m (mTools) -
// patient, study, series, orientation (the plane and size of the images), position (the place of the
// slice) and image. Beside the tree: what the chosen node is (its counts and main values), its image
// (the preview, its slider over the files under the node, with the cine) and its full info: the
// attributes all its files share, level by level (those of the image, those every image of its position
// shares, those of its orientation, of its series...), and those that vary in it.

// The value of a tag among the attributes (sorted by tag): put, or changed
static void SetAttribute(Attributes& a, Uint32 tag, Uint32 id)
{
    auto it = std::lower_bound(a.begin(), a.end(), tag, [](const std::pair<Uint32, Uint32>& x, Uint32 t) { return x.first < t; });
    if (it != a.end() && it->first == tag) it->second = id;
    else a.insert(it, { tag, id });
}

Person PersonOf(const std::string& name, const std::string& charset, const std::string& birth)
{
    Person p;
    for (char c : birth) if (c >= '0' && c <= '9') p.birth += c;
    const std::wstring w = Widen(name, charset, "\\=^");           // (a name: PN)
    // Decomposed, the accents are marks of their own (not letters), and go
    std::wstring d = w;
    const int n = NormalizeString(NormalizationD, w.c_str(), static_cast<int>(w.size()), nullptr, 0);
    if (n > 0)
    {
        d.assign(static_cast<size_t>(n), L'\0');
        const int k = NormalizeString(NormalizationD, w.c_str(), static_cast<int>(w.size()), &d[0], n);
        d.resize(k > 0 ? static_cast<size_t>(k) : 0);
    }
    // Its words (what is not a letter separates them), in alphabetical order: "JUAN GARCIA" is "GARCIA^JUAN"
    std::wstring word;
    for (wchar_t c : d + L" ")
    {
        if (IsCharAlphaW(c)) { word += c; continue; }
        if (word.empty()) continue;
        CharUpperBuffW(&word[0], static_cast<DWORD>(word.size()));
        p.words.push_back(word);
        word.clear();
    }
    std::sort(p.words.begin(), p.words.end());
    p.words.erase(std::unique(p.words.begin(), p.words.end()), p.words.end());
    for (const auto& w2 : p.words) p.name += (p.name.empty() ? L"" : L" ") + w2;
    return p;
}

// The same person, as far as they tell: what both have agrees. A missing birth date or name does not
// tell them apart, nor a name with fewer words than the other one (a second name, or surname, left out),
// nor an initial for a word ("R" for "RAVI"). With the same birth date in both, a word in common is enough
// (a name written two ways); and with only the year of the birth date apart, a word in common too (a
// mistake in the year, or an anonymizer that moves it)
bool SamePerson(const Person& a, const Person& b)
{
    auto common = [&]() {
        for (const auto& w : a.words) if (std::binary_search(b.words.begin(), b.words.end(), w)) return true;
        return false;
    };
    if (!a.birth.empty() && !b.birth.empty() && a.birth != b.birth)
        return a.birth.size() == 8 && b.birth.size() == 8 && a.birth.compare(4, 4, b.birth, 4, 4) == 0 && common();
    if (a.words.empty() || b.words.empty()) return true;
    if (!a.birth.empty() && a.birth == b.birth)
        for (const auto& w : a.words)
            if (std::binary_search(b.words.begin(), b.words.end(), w)) return true;
    const auto& few = a.words.size() <= b.words.size() ? a.words : b.words;
    const auto& many = a.words.size() <= b.words.size() ? b.words : a.words;
    std::vector<bool> used(many.size(), false);
    for (const auto& w : few)
    {
        size_t k = 0;
        for (; k < many.size(); k++)
        {
            if (used[k]) continue;
            const auto& x = many[k];
            if (x == w || (w.size() == 1 && x[0] == w[0]) || (x.size() == 1 && w[0] == x[0])) break;
        }
        if (k == many.size()) return false;
        used[k] = true;
    }
    return true;
}

// A file, if it is DICOM (a DICOMDIR is not one of the files): its header up to the pixels, by DCMTK.
// Values of more than 4 KB are not read: their size stands for them.
static Found GatherFile(const std::wstring& path, long long bytes, Gathered& g, std::vector<Gathered>& more, ValuePool& pool)
{
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Found::Other;
        unsigned char head[132];
        DWORD n = 0;
        const BOOL ok = ReadFile(h, head, sizeof(head), &n, nullptr);
        CloseHandle(h);
        if (!ok || !LooksDicom(head, n)) return Found::Other;
    }
    DcmFileFormat ff;
    if (ff.loadFileUntilTag(OFFilename(path.c_str(), OFTrue), EXS_Unknown, EGL_noChange, 4096, ERM_autoDetect, DCM_PixelData).bad())
        return Found::Other;
    DcmDataset* ds = ff.getDataset();
    auto get = [&](const DcmTagKey& key) {
        OFString v;
        ds->findAndGetOFStringArray(key, v);
        return std::string(v.c_str(), v.length());
    };
    if (get(DCM_SOPClassUID) == DirectorySopClass || ds->tagExists(DCM_DirectoryRecordSequence)) return Found::Directory;
    g.path = path;
    g.bytes = bytes;
    g.charset = get(DCM_SpecificCharacterSet);
    g.patient = get(DCM_PatientID);
    g.patientName = get(DCM_PatientName);           // (only to tell apart people that share a PatientID)
    g.birthDate = get(DCM_PatientBirthDate);
    g.study = get(DCM_StudyInstanceUID);
    g.studyDate = get(DCM_StudyDate);
    g.studyTime = get(DCM_StudyTime);
    g.studyDescription = get(DCM_StudyDescription);
    g.series = get(DCM_SeriesInstanceUID);
    g.seriesNumber = get(DCM_SeriesNumber);
    g.seriesDate = get(DCM_SeriesDate);
    g.seriesTime = get(DCM_SeriesTime);
    g.seriesDescription = get(DCM_SeriesDescription);
    g.modality = get(DCM_Modality);
    g.instance = get(DCM_InstanceNumber);
    g.trigger = get(DCM_TriggerTime);
    g.time = get(DCM_AcquisitionDateTime);
    if (g.time.empty()) g.time = get(DCM_AcquisitionDate) + get(DCM_AcquisitionTime);
    if (g.time.empty()) g.time = get(DCM_ContentDate) + get(DCM_ContentTime);
    g.orientationText = get(DCM_ImageOrientationPatient);
    g.positionText = get(DCM_ImagePositionPatient);
    g.oriented = Numbers(g.orientationText, g.orientation, 6);
    g.placed = Numbers(g.positionText, g.position, 3);
    Header h;
    h.imageOrientation = g.orientationText;
    h.imagePosition = g.positionText;
    g.hasZ = ZLevel(h, g.z);
    Uint16 rows = 0, columns = 0;
    ds->findAndGetUint16(DCM_Rows, rows);
    ds->findAndGetUint16(DCM_Columns, columns);
    g.rows = rows;
    g.columns = columns;
    Sint32 frames = 1;
    if (ds->findAndGetSint32(DCM_NumberOfFrames, frames).good() && frames > 1) g.frames = frames;
    // All the attributes: the meta information, then the dataset (in the order of their tags)
    for (DcmItem* item : { static_cast<DcmItem*>(ff.getMetaInfo()), static_cast<DcmItem*>(ds) })
        for (unsigned long i = 0; item && i < item->card(); i++)
        {
            DcmElement* e = item->getElement(i);
            if (!e) continue;
            const DcmTag& tag = e->getTag();
            std::wstring v;
            if (e->ident() == EVR_SQ)
            {
                const unsigned long k = static_cast<DcmSequenceOfItems*>(e)->card();
                v = L"(" + std::to_wstring(k) + (k == 1 ? L" item)" : L" items)");
            }
            else
            {
                v = ValueOf(e, item == ds ? g.charset : std::string());
                if (v.size() >= 2 && v.front() == L'[' && v.back() == L']') v = v.substr(1, v.size() - 2);
            }
            g.attributes.emplace_back((static_cast<Uint32>(tag.getGTag()) << 16) | tag.getETag(), pool.Id(v));
        }
    std::sort(g.attributes.begin(), g.attributes.end());

    // Enhanced multi-frame (a volume, a cine, in one file): each frame an image of its own, with the
    // position, orientation and trigger time of its functional groups (its own, or the shared ones)
    DcmSequenceOfItems* perFrame = nullptr;
    if (g.frames > 1 && ds->findAndGetSequence(DCM_PerFrameFunctionalGroupsSequence, perFrame).good() && perFrame &&
        static_cast<long>(perFrame->card()) == g.frames)
    {
        DcmSequenceOfItems* sharedSeq = nullptr;
        DcmItem* shared = ds->findAndGetSequence(DCM_SharedFunctionalGroupsSequence, sharedSeq).good() && sharedSeq && sharedSeq->card()
                              ? sharedSeq->getItem(0) : nullptr;
        auto inGroup = [](DcmItem* group, const DcmTagKey& sequence, const DcmTagKey& key) {
            DcmSequenceOfItems* s = nullptr;
            OFString v;
            if (group && group->findAndGetSequence(sequence, s).good() && s && s->card()) s->getItem(0)->findAndGetOFStringArray(key, v);
            return std::string(v.c_str(), v.length());
        };
        auto value = [&](DcmItem* item, const DcmTagKey& sequence, const DcmTagKey& key) {
            const std::string v = inGroup(item, sequence, key);
            return v.empty() ? inGroup(shared, sequence, key) : v;
        };
        const Gathered base = g;
        const long total = g.frames;
        more.reserve(static_cast<size_t>(total - 1));
        for (long k = 0; k < total; k++)
        {
            if (k > 0) more.push_back(base);
            Gathered& e = k == 0 ? g : more.back();
            DcmItem* item = perFrame->getItem(static_cast<unsigned long>(k));
            e.frame = static_cast<int>(k);
            e.frames = k == 0 ? total : 0;
            e.bytes = k == 0 ? bytes : 0;
            const std::string ipp = value(item, DCM_PlanePositionSequence, DCM_ImagePositionPatient);
            const std::string iop = value(item, DCM_PlaneOrientationSequence, DCM_ImageOrientationPatient);
            const std::string delay = value(item, DCM_CardiacSynchronizationSequence, DCM_NominalCardiacTriggerDelayTime);
            if (!ipp.empty()) e.positionText = ipp;
            if (!iop.empty()) e.orientationText = iop;
            if (!delay.empty()) e.trigger = delay;
            e.oriented = Numbers(e.orientationText, e.orientation, 6);
            e.placed = Numbers(e.positionText, e.position, 3);
            Header fh;
            fh.imageOrientation = e.orientationText;
            fh.imagePosition = e.positionText;
            e.hasZ = ZLevel(fh, e.z);
            // (and among its attributes, where the full info shows them varying from frame to frame)
            if (!ipp.empty()) SetAttribute(e.attributes, 0x00200032, pool.Id(Widen(ipp)));
            if (!iop.empty()) SetAttribute(e.attributes, 0x00200037, pool.Id(Widen(iop)));
            if (!delay.empty()) SetAttribute(e.attributes, 0x00181060, pool.Id(Widen(delay)));
            const std::string spacing = value(item, DCM_PixelMeasuresSequence, DCM_PixelSpacing);
            if (!spacing.empty()) SetAttribute(e.attributes, 0x00280030, pool.Id(Widen(spacing)));
        }
    }
    return Found::Dicom;
}

void Gather(Gathering* g)
{
    const ULONGLONG start = GetTickCount64();
    struct Item { std::wstring path; long long bytes; };
    std::vector<Item> items;
    // The folders depth first, in the order of their names (as Explorer shows them)
    std::vector<std::wstring> folders{ g->root };
    while (!folders.empty() && !g->cancel)
    {
        const std::wstring folder = std::move(folders.back());
        folders.pop_back();
        const std::wstring base = folder + (!folder.empty() && folder.back() == L'\\' ? L"" : L"\\");
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> inside;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // (not into links: they can loop)
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..") && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    inside.push_back(base + fd.cFileName);
                continue;
            }
            // Not downloaded (cloud files): reading them would download them
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) { g->offline++; continue; }
            items.push_back({ base + fd.cFileName, (static_cast<long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
            g->found++;
        } while (!g->cancel && FindNextFileW(find, &fd));
        FindClose(find);
        std::sort(inside.begin(), inside.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) > 0; });
        folders.insert(folders.end(), inside.begin(), inside.end());
    }
    g->listing = false;

    std::vector<Gathered> read(items.size());
    std::vector<std::vector<Gathered>> frames(items.size());     // (the frames of enhanced files after their first one)
    std::vector<Found> kinds(items.size(), Found::Other);
    std::atomic<size_t> next{ 0 };
    auto work = [&]() {
        for (;;)
        {
            const size_t i = next++;
            if (i >= items.size() || g->cancel) break;
            try { kinds[i] = GatherFile(items[i].path, items[i].bytes, read[i], frames[i], g->pool); }
            catch (...) { kinds[i] = Found::Other; }
            g->done++;
        }
    };
    unsigned n = std::thread::hardware_concurrency();
    n = n < 2 ? 2 : n > 6 ? 6 : n;
    std::vector<std::thread> workers;
    for (unsigned k = 0; k < n; k++) workers.emplace_back(work);
    for (auto& w : workers) w.join();
    for (size_t i = 0; i < items.size(); i++)
    {
        if (kinds[i] == Found::Dicom) 
        {
            g->files.push_back(std::move(read[i]));
            for (auto& f : frames[i]) g->files.push_back(std::move(f));
        }
        else if (kinds[i] == Found::Directory) g->directories++;
        else g->others++;
    }
    g->seconds = (GetTickCount64() - start) / 1000.0;
    if (!g->cancel) PostMessageW(g->window, g->message, 0, 0);
}

// Values that say who the patient is (and the names of the staff): hidden unless asked for
bool IdentifyingTag(Uint32 tag)
{
    static const Uint32 tags[] = {
        0x00100010, 0x00100030, 0x00100032, 0x00101000, 0x00101001, 0x00101002, 0x00101005, 0x00101040,
        0x00101060, 0x00102154, 0x00102155, 0x001021B0, 0x00080090, 0x00080092, 0x00080094, 0x00080081,
        0x00081048, 0x00081050, 0x00081060, 0x00081070, 0x00321032, 0x00400006, 0x00380400, 0x00404037 };
    return std::find(std::begin(tags), std::end(tags), tag) != std::end(tags);
}

std::wstring TagText(Uint32 tag)
{
    wchar_t s[16];
    swprintf_s(s, L"(%04X,%04X)", tag >> 16, tag & 0xFFFF);
    return s;
}

std::wstring TagName(Uint32 tag)
{
    DcmTag t(static_cast<Uint16>(tag >> 16), static_cast<Uint16>(tag & 0xFFFF));
    std::wstring name = Widen(t.getTagName());
    if (name == Widen(DcmTag_ERROR_TagName)) name.clear();
    if (name.empty() && ((tag >> 16) & 1)) name = L"(private)";
    return name;
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

void CopyText(HWND owner, const std::wstring& text)
{
    if (text.empty() || !OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory)
    {
        memcpy(GlobalLock(memory), text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

// Explorer windows of the folders of these files, with them selected (the first ten folders)
void ShowInExplorer(const std::vector<std::wstring>& paths)
{
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> folders;
    for (const auto& p : paths)
    {
        const size_t slash = p.find_last_of(L'\\');
        if (slash == std::wstring::npos) continue;
        const std::wstring folder = p.substr(0, slash == 2 ? 3 : slash);
        auto it = std::find_if(folders.begin(), folders.end(), [&](const auto& f) { return f.first == folder; });
        if (it == folders.end())
        {
            if (folders.size() >= 10) continue;
            folders.push_back({ folder, {} });
            it = folders.end() - 1;
        }
        it->second.push_back(p);
    }
    for (const auto& f : folders)
    {
        PIDLIST_ABSOLUTE dir = ILCreateFromPathW(f.first.c_str());
        if (!dir) continue;
        std::vector<PIDLIST_ABSOLUTE> items;
        std::vector<PCUITEMID_CHILD> children;
        for (const auto& p : f.second)
        {
            if (items.size() >= 4000) break;
            if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(p.c_str()))
            {
                items.push_back(item);
                children.push_back(ILFindLastID(item));
            }
        }
        SHOpenFolderAndSelectItems(dir, static_cast<UINT>(children.size()), children.data(), 0);
        for (auto item : items) ILFree(item);
        ILFree(dir);
    }
}
