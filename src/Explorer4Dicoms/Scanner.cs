// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the count: the DICOM files of a folder, by study and series, its folders commented

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Explorer4Dicoms
{
    public static class Scanner
    {
        // indexDir: folder of the index of counted locations (null: no cross-location check)
        public static Task<ScanResult> StartScan(string root, int threads, ScanProgress p, bool writeComments, string indexDir)
        {
            return Task.Factory.StartNew(() => Scan(root, threads, p, writeComments, indexDir), TaskCreationOptions.LongRunning);
        }

        public static ScanResult Scan(string root, int threads, ScanProgress p, bool writeComments, string indexDir)
        {
            var instances = new ConcurrentBag<SeriesInstance>();                   // InstanceNumber of every image, per series and folder
            var indexEntries = new ConcurrentBag<IndexEntry>();                    // what this count leaves in the index
            var studies = new ConcurrentDictionary<string, StudyAgg>();
            var folders = new ConcurrentBag<FolderStudies>();
            var seriesDescriptions = new ConcurrentDictionary<string, string>();   // SeriesInstanceUID -> SeriesDescription
            var seriesNumbers = new ConcurrentDictionary<string, string>();        // SeriesInstanceUID -> SeriesNumber
            var sopFirstFile = new ConcurrentDictionary<string, string>();         // "StudyUID|SOPInstanceUID" -> first file seen
            var sopCopies = new ConcurrentBag<KeyValuePair<string, string>>();     // same key, another file with it

            // 1) Mark as "counting" what Explorer is showing (the folder and its subfolders) before
            //    anything else: marking costs ~10 ms per folder, too slow for the whole tree up front
            var marked = System.Diagnostics.Stopwatch.StartNew();
            if (writeComments)
            {
                TrySetComment(root, FolderComments.CountingText, p);
                try
                {
                    foreach (var d in Directory.GetDirectories(root))
                        if (!string.Equals(Path.GetFileName(d), "_DicomQuarantine", StringComparison.OrdinalIgnoreCase))
                            TrySetComment(d, FolderComments.CountingText, p);
                }
                catch (Exception) { }   // reported as inaccessible by BuildTree
            }

            // 2) Full folder tree (pre-order) and list of files grouped by folder
            List<FolderNode> nodes = BuildTree(root, p.InaccessibleDirs);
            var work = new List<KeyValuePair<FolderNode, string>>();
            foreach (var n in nodes)
                foreach (var f in n.Files) work.Add(new KeyValuePair<FolderNode, string>(n, f));
            p.TotalFiles = work.Count;

            // The deeper folders are marked in the background while counting, shallowest first;
            // the folders already finished are skipped
            Task marker = null;
            if (writeComments)
            {
                // Explorer only repaints after ~0.6 s without change notifications; without this
                // pause "Counting..." would run straight into the results and never be shown
                int wait = 1000 - (int)marked.ElapsedMilliseconds;
                if (wait > 0) Thread.Sleep(wait);
                p.CountingShown = true;
                var deeper = nodes.FindAll(n => n.Depth >= 2);
                deeper.Sort((a, b) => a.Depth.CompareTo(b.Depth));     // the order within a level does not matter
                // A few threads, so the marking does not take them all from the reading
                var markOpts = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, threads / 4) };
                marker = Task.Factory.StartNew(() =>
                    Parallel.ForEach(Partitioner.Create(deeper, true), markOpts, n =>
                    {
                        lock (n.CommentSync)
                            if (!n.Finished) TrySetComment(n.Path, FolderComments.CountingText, p);
                    }), TaskCreationOptions.LongRunning);
            }

            // A folder is finished once all its files and subfolders are finished
            Action<FolderNode> finish = null;
            finish = node =>
            {
                // From here on the background marking no longer writes "Counting..." over it
                lock (node.CommentSync) node.Finished = true;
                string single = null;
                foreach (var u in node.Uids) { single = u; break; }
                node.FinalStudies = node.Uids.Count;
                node.FinalUid = node.Uids.Count == 1 ? single : null;
                node.FinalSeries = node.SeriesUids.Count;
                folders.Add(new FolderStudies
                {
                    Path = node.Path,
                    Studies = node.Uids.Count,
                    Series = node.SeriesUids.Count,
                    Files = node.DicomFiles,
                    StudyInstanceUID = single
                });
                if (writeComments)
                {
                    // Without DICOM: "---" when the whole subtree has no files at all
                    if (node.Uids.Count == 0)
                        TrySetComment(node.Path, node.TotalFiles == 0 ? FolderComments.EmptyText : FolderComments.NoDicomText(node.TotalFiles), p, "");
                    else
                    {
                        // Icon colors: one per study, in date order
                        var byDate = new List<string>(node.Uids);
                        byDate.Sort((a, b) => string.CompareOrdinal(studies[a].StudyDate + a, studies[b].StudyDate + b));
                        node.IconColors = byDate.ConvertAll<int>(FolderIcons.ColorIndex);
                        string seriesNumber = null, seriesDescription = null;
                        if (node.SeriesUids.Count == 1)
                            foreach (var s in node.SeriesUids)
                            {
                                seriesNumbers.TryGetValue(s, out seriesNumber);
                                seriesDescriptions.TryGetValue(s, out seriesDescription);
                            }
                        // Best guess of the study's modality so far; fixed in the final pass if it changes
                        string tag = null, studyDate = null;
                        if (node.Uids.Count == 1)
                        {
                            tag = FolderComments.StudyTag(single, studies[single].MainModality());
                            studyDate = studies[single].StudyDate;
                        }
                        node.BaseComment = FolderComments.Format(node.Uids.Count, tag, studyDate, node.SeriesUids.Count,
                            seriesNumber, seriesDescription, node.DicomFiles, node.TotalFiles);
                        TrySetComment(node.Path, node.BaseComment, p, TryIcon(node.IconColors));
                    }
                }
                var parent = node.Parent;
                if (parent == null) return;
                lock (parent)
                {
                    parent.Uids.UnionWith(node.Uids);
                    parent.SeriesUids.UnionWith(node.SeriesUids);
                    parent.DicomFiles += node.DicomFiles;
                    parent.TotalFiles += node.TotalFiles;
                }
                node.Uids.Clear();
                node.SeriesUids.Clear();
                if (Interlocked.Decrement(ref parent.Pending) == 0) finish(parent);
            };

            // Empty folders: finished right away
            foreach (var n in nodes)
                if (n.Files.Length == 0 && n.ChildCount == 0) finish(n);

            // 3) Parallel reading; the load-balancing partitioner takes files in order, so folders
            //    complete one after another instead of all at the end
            var opts = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, threads) };
            Parallel.ForEach(Partitioner.Create(work, true), opts, item =>
            {
                string path = item.Value;
                FolderNode node = item.Key;
                DicomTags tags = null;
                bool failed = false;
                try { tags = DicomParser.ReadFile(path); }
                catch (Exception) { failed = true; }

                if (failed) Interlocked.Increment(ref p.Errors);
                else if (tags == null) Interlocked.Increment(ref p.NotDicom);
                else if (string.IsNullOrEmpty(tags.Uid)) Interlocked.Increment(ref p.NoStudy);
                else
                {
                    Interlocked.Increment(ref p.Dicom);
                    StudyAgg agg;
                    if (!studies.TryGetValue(tags.Uid, out agg))
                    {
                        var created = new StudyAgg(tags.Uid);
                        if (studies.TryAdd(tags.Uid, created)) { agg = created; Interlocked.Increment(ref p.Studies); }
                        else agg = studies[tags.Uid];
                    }
                    agg.Add(tags);
                    // Duplicate candidates: same study AND same image (StudyInstanceUID + SOPInstanceUID)
                    if (!string.IsNullOrEmpty(tags.SopUid))
                    {
                        string imageKey = tags.Uid + "|" + tags.SopUid;
                        if (!sopFirstFile.TryAdd(imageKey, path))
                            sopCopies.Add(new KeyValuePair<string, string>(imageKey, path));
                        if (indexDir != null)
                        {
                            var fi = new FileInfo(path);
                            indexEntries.Add(new IndexEntry { Path = path, Size = fi.Length, Ticks = fi.LastWriteTimeUtc.Ticks, Study = tags.Uid, Sop = tags.SopUid });
                        }
                    }
                    if (!string.IsNullOrEmpty(tags.SeriesUid))
                    {
                        int number;
                        if (int.TryParse(tags.InstanceNumber, out number))
                            instances.Add(new SeriesInstance { Key = tags.Uid + "|" + tags.SeriesUid, Dir = node.Path, Number = number });
                        if (!string.IsNullOrEmpty(tags.SeriesDescription)) seriesDescriptions.TryAdd(tags.SeriesUid, tags.SeriesDescription);
                        if (!string.IsNullOrEmpty(tags.SeriesNumber)) seriesNumbers.TryAdd(tags.SeriesUid, tags.SeriesNumber);
                    }
                    lock (node)
                    {
                        node.Uids.Add(tags.Uid);
                        if (!string.IsNullOrEmpty(tags.SeriesUid)) node.SeriesUids.Add(tags.SeriesUid);
                        node.DicomFiles++;
                    }
                }
                Interlocked.Increment(ref p.Files);
                if (Interlocked.Decrement(ref node.Pending) == 0) finish(node);
            });
            // Every folder is finished by now, so the marking only has skips left
            if (marker != null) marker.Wait();

            var list = new List<Study>();
            foreach (var agg in studies.Values) list.Add(agg.ToStudy());
            list.Sort((a, b) => string.CompareOrdinal(a.StudyDate + a.StudyInstanceUID, b.StudyDate + b.StudyInstanceUID));

            var folderList = new List<FolderStudies>(folders);
            folderList.Sort((a, b) => StringComparer.OrdinalIgnoreCase.Compare(a.Path, b.Path));
            var result = new ScanResult { Counts = p, Studies = list, Folders = folderList };
            var studyTags = new Dictionary<string, string>();
            foreach (var kv in studies) studyTags[kv.Key] = FolderComments.StudyTag(kv.Key, kv.Value.MainModality());
            Func<string, string> seriesLabel = seriesUid =>
            {
                string number, description;
                seriesNumbers.TryGetValue(seriesUid, out number);
                seriesDescriptions.TryGetValue(seriesUid, out description);
                int n;
                string label = int.TryParse(number, out n) ? n.ToString("D3") : (number ?? "");
                if (!string.IsNullOrEmpty(description)) label = label == "" ? description : label + "." + description;
                return label == "" ? "(unnamed series)" : label;
            };
            FindProblems(root, nodes, sopFirstFile, sopCopies, studyTags, result, writeComments, p,
                         new List<SeriesInstance>(instances), seriesLabel, new List<IndexEntry>(indexEntries), indexDir);
            return result;
        }

        // 4) What needs the whole tree: the final study tags (a study's main modality is only known
        //    once all its files are read), studies split across folders, duplicated files, incomplete
        //    series and copies in other counted locations. The affected folders get their comment rewritten.
        static void FindProblems(string root, List<FolderNode> nodes, ConcurrentDictionary<string, string> sopFirstFile,
                                 ConcurrentBag<KeyValuePair<string, string>> sopCopies, Dictionary<string, string> studyTags,
                                 ScanResult result, bool writeComments, ScanProgress p,
                                 List<SeriesInstance> instances, Func<string, string> seriesLabel,
                                 List<IndexEntry> indexEntries, string indexDir)
        {
            var nodeByPath = new Dictionary<string, FolderNode>(StringComparer.OrdinalIgnoreCase);
            foreach (var n in nodes) nodeByPath[n.Path] = n;

            FindIncompleteSeries(instances, studyTags, seriesLabel, nodeByPath, result);
            result.External = new List<ExternalCopySet>();
            // Every file with copies -> its copies, for the "duplicated in" column of the files
            var fileCopies = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            if (indexDir != null) CheckOtherLocations(root, indexDir, indexEntries, nodeByPath, result, fileCopies);

            // Duplicated files: every SOPInstanceUID seen in more than one file
            var copies = new Dictionary<string, List<string>>();
            foreach (var kv in sopCopies)
            {
                List<string> files;
                if (!copies.TryGetValue(kv.Key, out files))
                {
                    files = new List<string> { sopFirstFile[kv.Key] };
                    copies[kv.Key] = files;
                }
                files.Add(kv.Value);
            }
            // Same image copied (identical bytes) or re-processed/anonymized (different bytes)?
            // (candidates already share StudyInstanceUID and SOPInstanceUID)
            var identical = new ConcurrentDictionary<string, bool>();
            Parallel.ForEach(copies, kv => identical[kv.Key] = FileCompare.AllIdentical(kv.Value));

            var sets = new Dictionary<string, DuplicateSet>(StringComparer.OrdinalIgnoreCase);
            int duplicatedFiles = 0;
            foreach (var kv in copies)
            {
                var files = kv.Value;
                bool same = identical[kv.Key];
                var dirs = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var f in files)
                {
                    foreach (var other in files)
                        if (!string.Equals(other, f, StringComparison.OrdinalIgnoreCase)) AddCopy(fileCopies, f, other, same);
                    string dir = Path.GetDirectoryName(f);
                    dirs.Add(dir);
                    duplicatedFiles++;
                    FolderNode node;
                    if (nodeByPath.TryGetValue(dir, out node))
                        for (var n = node; n != null; n = n.Parent)
                        {
                            n.DuplicatedFiles++;
                            if (!same) n.DifferentFiles++;
                        }
                }
                string key = string.Join("|", dirs);
                DuplicateSet set;
                if (!sets.TryGetValue(key, out set))
                {
                    set = new DuplicateSet { Folders = new List<string>(dirs), Groups = new List<DuplicateGroup>() };
                    sets[key] = set;
                }
                set.Files += files.Count;
                if (same) set.Identical += files.Count; else set.Different += files.Count;
                set.Groups.Add(new DuplicateGroup { Identical = same, Files = files });
            }
            foreach (var set in sets.Values) set.Keep = SuggestKeep(set.Folders, nodeByPath);
            result.Duplicates = new List<DuplicateSet>(sets.Values);
            result.Duplicates.Sort((a, b) => b.Files.CompareTo(a.Files));
            result.DuplicatedFiles = duplicatedFiles;
            if (indexDir != null)
                GlobalIndex.SaveCopies(Path.Combine(Path.GetDirectoryName(indexDir), "duplicates"), root, fileCopies);

            // Split studies: a study whose topmost single-study folders are more than one
            var locations = new Dictionary<string, List<FolderNode>>();
            foreach (var n in nodes)
            {
                if (n.FinalUid == null || (n.Parent != null && n.Parent.FinalStudies == 1)) continue;
                List<FolderNode> l;
                if (!locations.TryGetValue(n.FinalUid, out l)) { l = new List<FolderNode>(); locations[n.FinalUid] = l; }
                l.Add(n);
            }
            result.Splits = new List<StudySplit>();
            var splitOthers = new Dictionary<FolderNode, List<FolderNode>>();
            foreach (var kv in locations)
            {
                if (kv.Value.Count < 2) continue;
                var paths = new List<string>();
                foreach (var n in kv.Value) paths.Add(n.Path);
                result.Splits.Add(new StudySplit { Tag = studyTags[kv.Key], StudyInstanceUID = kv.Key, Folders = paths });
                foreach (var n in kv.Value) splitOthers[n] = kv.Value.FindAll(o => o != n);
            }

            if (!writeComments) return;
            var rewrites = new List<KeyValuePair<string, string>>();    // folder -> comment with warnings
            foreach (var n in nodes)
            {
                if (n.BaseComment == null) continue;
                bool changed = false;
                if (n.FinalUid != null)
                {
                    string expected = "[" + studyTags[n.FinalUid] + "]";
                    if (!n.BaseComment.StartsWith(expected, StringComparison.Ordinal))
                    {
                        n.BaseComment = expected + n.BaseComment.Substring(n.BaseComment.IndexOf(']') + 1);
                        changed = true;
                    }
                }
                var warn = new StringBuilder();
                List<FolderNode> others;
                if (splitOthers.TryGetValue(n, out others))
                {
                    var names = new List<string>();
                    foreach (var o in others.GetRange(0, Math.Min(3, others.Count))) names.Add(Relative(root, o.Path));
                    warn.Append("  " + FolderComments.Warning + " also in: " + string.Join(", ", names));
                    if (others.Count > 3) warn.Append(" (+" + (others.Count - 3) + " more)");
                }
                if (n.DuplicatedFiles > 0)
                {
                    warn.Append("  " + FolderComments.Warning + " " + n.DuplicatedFiles + " duplicated");
                    if (n.DifferentFiles > 0) warn.Append(" (" + n.DifferentFiles + " differ)");
                }
                if (n.IncompleteNote != null && n.FinalSeries == 1)
                    warn.Append("  " + FolderComments.Warning + " " + n.IncompleteNote);
                else if (n.IncompleteSeries != null && n.IncompleteSeries.Count > 0)
                    warn.Append("  " + FolderComments.Warning + " " + n.IncompleteSeries.Count + " incomplete series");
                if (n.ExternalFiles > 0)
                {
                    warn.Append("  " + FolderComments.Warning + " " + n.ExternalFiles + " also in other locations");
                    if (n.ExternalDifferent > 0) warn.Append(" (" + n.ExternalDifferent + " differ)");
                }
                if (warn.Length > 0 || changed) rewrites.Add(new KeyValuePair<string, string>(n.Path, n.BaseComment + warn));
            }
            Parallel.ForEach(rewrites, kv => TrySetComment(kv.Key, kv.Value, p));
        }

        // Incomplete series: for every series, the range of InstanceNumbers seen in the whole tree
        // (min..max) is what each folder holding that series should have; a folder with a partial
        // copy is flagged even if another folder has the whole series. Gaps before the lowest number
        // seen cannot be known, so numbering that starts at 5 (or 1001) is not reported.
        static void FindIncompleteSeries(List<SeriesInstance> instances, Dictionary<string, string> studyTags,
                                         Func<string, string> seriesLabel, Dictionary<string, FolderNode> nodeByPath,
                                         ScanResult result)
        {
            result.Incomplete = new List<IncompleteSeries>();
            var bySeries = new Dictionary<string, Dictionary<string, HashSet<int>>>();
            foreach (var i in instances)
            {
                Dictionary<string, HashSet<int>> dirs;
                if (!bySeries.TryGetValue(i.Key, out dirs))
                {
                    dirs = new Dictionary<string, HashSet<int>>(StringComparer.OrdinalIgnoreCase);
                    bySeries[i.Key] = dirs;
                }
                HashSet<int> numbers;
                if (!dirs.TryGetValue(i.Dir, out numbers)) { numbers = new HashSet<int>(); dirs[i.Dir] = numbers; }
                numbers.Add(i.Number);
            }
            foreach (var kv in bySeries)
            {
                int min = int.MaxValue, max = int.MinValue;
                foreach (var numbers in kv.Value.Values)
                    foreach (var n in numbers) { if (n < min) min = n; if (n > max) max = n; }
                long expected = (long)max - min + 1;
                if (expected > 100000) continue;        // implausible numbering: not judged
                string studyUid = kv.Key.Substring(0, kv.Key.IndexOf('|'));
                string seriesUid = kv.Key.Substring(kv.Key.IndexOf('|') + 1);
                foreach (var dir in kv.Value)
                {
                    int missing = (int)expected - dir.Value.Count;
                    if (missing <= 0) continue;
                    result.Incomplete.Add(new IncompleteSeries
                    {
                        Tag = studyTags[studyUid],
                        Series = seriesLabel(seriesUid),
                        Folder = dir.Key,
                        Have = dir.Value.Count,
                        Expected = (int)expected,
                        Missing = Ranges(dir.Value, min, max)
                    });
                    FolderNode node;
                    if (!nodeByPath.TryGetValue(dir.Key, out node)) continue;
                    node.IncompleteNote = missing + " missing (" + dir.Value.Count + " of " + expected + ")";
                    for (var n = node; n != null; n = n.Parent)
                    {
                        if (n.IncompleteSeries == null) n.IncompleteSeries = new HashSet<string>();
                        n.IncompleteSeries.Add(kv.Key);
                    }
                }
            }
            result.Incomplete.Sort((a, b) => StringComparer.OrdinalIgnoreCase.Compare(a.Folder, b.Folder));
        }

        // Numbers in min..max that are not in the set, as ranges: "4, 7-9, 15" (at most 10 ranges)
        static string Ranges(HashSet<int> have, int min, int max)
        {
            var parts = new List<string>();
            int count = 0;
            for (int n = min; n <= max; n++)
            {
                if (have.Contains(n)) continue;
                int start = n;
                while (n + 1 <= max && !have.Contains(n + 1)) n++;
                count++;
                if (parts.Count < 10) parts.Add(start == n ? start.ToString() : start + "-" + n);
            }
            string text = string.Join(", ", parts);
            if (count > parts.Count) text += ", ...";
            return text;
        }

        // Copies in other counted locations. Every count saves its own index: path (name included),
        // size, date and time of each DICOM, with its StudyInstanceUID and SOPInstanceUID. Here the
        // current files are matched against the other indexes (same study + SOP), and every match is
        // first checked against the disk: a file deleted, or replaced by another one (different size,
        // date or time), leaves the index. Confirmed with the same byte-exact comparison as duplicates.
        static void CheckOtherLocations(string root, string indexDir, List<IndexEntry> entries,
                                        Dictionary<string, FolderNode> nodeByPath, ScanResult result,
                                        Dictionary<string, List<string>> fileCopies)
        {
            string rootSlash = root.TrimEnd('\\') + "\\";
            string ownIndex = GlobalIndex.FileFor(indexDir, root);
            var current = new Dictionary<string, List<string>>();
            foreach (var e in entries)
            {
                List<string> files;
                string key = e.Study + "|" + e.Sop;
                if (!current.TryGetValue(key, out files)) { files = new List<string>(); current[key] = files; }
                files.Add(e.Path);
            }

            var sets = new Dictionary<string, ExternalCopySet>(StringComparer.OrdinalIgnoreCase);
            var matched = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);   // current file -> has an identical copy
            if (Directory.Exists(indexDir))
            {
                foreach (var file in Directory.GetFiles(indexDir, "*.tsv"))
                {
                    if (string.Equals(file, ownIndex, StringComparison.OrdinalIgnoreCase)) continue;
                    string otherRoot;
                    var list = GlobalIndex.Load(file, out otherRoot);
                    if (list == null) continue;
                    // A location inside this one: this count replaces it
                    if ((otherRoot.TrimEnd('\\') + "\\").StartsWith(rootSlash, StringComparison.OrdinalIgnoreCase))
                    {
                        File.Delete(file);
                        continue;
                    }
                    bool changed = false;
                    var kept = new List<IndexEntry>();
                    foreach (var e in list)
                    {
                        // Files inside this location (when the other one contains it): this count replaces them
                        if (e.Path.StartsWith(rootSlash, StringComparison.OrdinalIgnoreCase)) { changed = true; continue; }
                        List<string> mine;
                        if (current.TryGetValue(e.Study + "|" + e.Sop, out mine))
                        {
                            var fi = new FileInfo(e.Path);
                            if (!fi.Exists || fi.Length != e.Size || fi.LastWriteTimeUtc.Ticks != e.Ticks) { changed = true; continue; }
                            foreach (var m in mine)
                            {
                                bool same = FileCompare.Identical(m, e.Path);
                                AddCopy(fileCopies, m, e.Path, same);
                                string myDir = Path.GetDirectoryName(m), otherDir = Path.GetDirectoryName(e.Path);
                                ExternalCopySet set;
                                string key = myDir + "|" + otherDir;
                                if (!sets.TryGetValue(key, out set))
                                {
                                    set = new ExternalCopySet { Folder = myDir, OtherFolder = otherDir };
                                    sets[key] = set;
                                }
                                set.Files++;
                                if (same) set.Identical++; else set.Different++;
                                bool before;
                                matched[m] = (matched.TryGetValue(m, out before) && before) || same;
                            }
                        }
                        kept.Add(e);
                    }
                    if (changed) GlobalIndex.Save(file, otherRoot, kept);
                }
            }
            // Each file counts once in its folders, however many copies it has elsewhere;
            // it counts as "differ" only if none of its copies is byte-identical
            foreach (var kv in matched)
            {
                FolderNode node;
                if (!nodeByPath.TryGetValue(Path.GetDirectoryName(kv.Key), out node)) continue;
                for (var n = node; n != null; n = n.Parent)
                {
                    n.ExternalFiles++;
                    if (!kv.Value) n.ExternalDifferent++;
                }
            }
            GlobalIndex.Save(ownIndex, root, entries);
            result.External = new List<ExternalCopySet>(sets.Values);
            result.External.Sort((a, b) => b.Files.CompareTo(a.Files));
        }

        // A copy of a file (the same study and image); one whose bytes differ is marked so
        static void AddCopy(Dictionary<string, List<string>> fileCopies, string file, string copy, bool identical)
        {
            List<string> list;
            if (!fileCopies.TryGetValue(file, out list)) { list = new List<string>(); fileCopies[file] = list; }
            list.Add(identical ? copy : copy + " (differs)");
        }

        // The folder whose study is most "at home" there: the one whose topmost single-study
        // folder holds the most files
        static string SuggestKeep(List<string> folders, Dictionary<string, FolderNode> nodeByPath)
        {
            string best = folders[0];
            int bestFiles = -1;
            foreach (var f in folders)
            {
                FolderNode n;
                if (!nodeByPath.TryGetValue(f, out n)) continue;
                while (n.Parent != null && n.Parent.FinalStudies == 1) n = n.Parent;
                if (n.TotalFiles > bestFiles) { bestFiles = n.TotalFiles; best = f; }
            }
            return best;
        }

        static string Relative(string root, string path)
        {
            if (path.Length <= root.Length) return "(root)";
            return path.Substring(root.Length).TrimStart('\\');
        }

        static void TrySetComment(string dir, string text, ScanProgress p, string icon = null)
        {
            try { FolderComments.Set(dir, text, icon); }
            catch (Exception) { Interlocked.Increment(ref p.CommentErrors); }
        }

        // Path of the folder icon for those study colors (built if needed); null (icon left as it
        // is) if it cannot be built
        static string TryIcon(List<int> colors)
        {
            if (colors == null || colors.Count == 0) return null;
            try { return FolderIcons.Ensure(FolderIcons.NameFor(colors)); }
            catch (Exception) { return null; }
        }

        static List<FolderNode> BuildTree(string root, List<string> inaccessible)
        {
            var nodes = new List<FolderNode>();
            var stack = new Stack<KeyValuePair<FolderNode, int>>();
            stack.Push(new KeyValuePair<FolderNode, int>(new FolderNode(root, null), 0));
            while (stack.Count > 0)
            {
                var item = stack.Pop();
                var node = item.Key;
                nodes.Add(node);
                string[] dirs;
                try
                {
                    // desktop.ini is written by this very script: not counted as a file
                    node.Files = Array.FindAll(Directory.GetFiles(node.Path),
                        f => !string.Equals(Path.GetFileName(f), "desktop.ini", StringComparison.OrdinalIgnoreCase));
                    // The quarantine folder of this script is never counted
                    dirs = Array.FindAll(Directory.GetDirectories(node.Path),
                        d => !string.Equals(Path.GetFileName(d), "_DicomQuarantine", StringComparison.OrdinalIgnoreCase));
                }
                catch (Exception)
                {
                    inaccessible.Add(node.Path);
                    dirs = new string[0];
                }
                // Depth limit as protection against symbolic link loops
                if (item.Value >= 100) dirs = new string[0];
                // Pushed in reverse so the stack pops them in alphabetical order
                for (int i = dirs.Length - 1; i >= 0; i--)
                    stack.Push(new KeyValuePair<FolderNode, int>(new FolderNode(dirs[i], node), item.Value + 1));
                node.Depth = item.Value;
                node.ChildCount = dirs.Length;
                node.Pending = node.Files.Length + dirs.Length;
                node.TotalFiles = node.Files.Length;     // subfolders are added as they finish
            }
            return nodes;
        }
    }

    internal class FolderNode
    {
        public readonly string Path;
        public readonly FolderNode Parent;
        public string[] Files = new string[0];
        public int Depth;           // 0 = the counted folder
        public bool Finished;       // its final comment is (being) written; guarded by CommentSync
        public readonly object CommentSync = new object();
        public int ChildCount;
        public int Pending;         // files + subfolders still to finish
        public int DicomFiles;      // DICOM files with StudyInstanceUID in the whole subtree
        public int TotalFiles;      // all files in the whole subtree (desktop.ini excluded)
        public int FinalStudies;    // distinct studies in the subtree, once finished
        public string FinalUid;     // the StudyInstanceUID when FinalStudies == 1
        public string BaseComment;  // comment without warnings
        public List<int> IconColors;    // palette index of every study of the subtree, in date order
        public int DuplicatedFiles; // files in the subtree whose SOPInstanceUID also appears in another file
        public int DifferentFiles;  // ...of which the copies do not have identical contents
        public int FinalSeries;     // distinct series in the subtree, once finished
        public string IncompleteNote;               // "N missing (x of y)" for a series in this very folder
        public HashSet<string> IncompleteSeries;    // incomplete series anywhere in the subtree
        public int ExternalFiles;   // files in the subtree with a copy in another counted location
        public int ExternalDifferent;
        public readonly HashSet<string> Uids = new HashSet<string>();
        public readonly HashSet<string> SeriesUids = new HashSet<string>();

        public FolderNode(string path, FolderNode parent) { Path = path; Parent = parent; }
    }

    internal class SeriesInstance
    {
        public string Key;          // StudyInstanceUID|SeriesInstanceUID
        public string Dir;
        public int Number;          // InstanceNumber
    }

    internal class IndexEntry
    {
        public string Path;
        public long Size;
        public long Ticks;          // last write time (UTC)
        public string Study, Sop;
    }
}
