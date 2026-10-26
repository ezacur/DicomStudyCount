// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the duplicates, the data of the counts, and the global index of the counted locations

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
    // "Show DICOM duplicates" on a file: its copies found by the counts, shown selected in Explorer
    public static class Duplicates
    {
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr ILCreateFromPathW(string path);

        [DllImport("shell32.dll")]
        static extern void ILFree(IntPtr pidl);

        [DllImport("shell32.dll")]
        static extern int SHOpenFolderAndSelectItems(IntPtr folder, uint count, IntPtr[] items, uint flags);

        // The copies of a file ("path (differs)" when not byte-identical), or none if no count found any
        public static string[] Of(string copiesDir, string file)
        {
            if (!Directory.Exists(copiesDir)) return new string[0];
            string prefix = file + "\t";
            foreach (var tsv in Directory.GetFiles(copiesDir, "*.tsv"))
            {
                string root = GlobalIndex.RootOf(tsv);
                if (root == null || !Paths.IsUnder(file, root)) continue;
                foreach (var line in File.ReadLines(tsv, Encoding.UTF8))
                    if (line.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    {
                        var f = line.Split('\t');
                        var copies = new string[f.Length - 1];
                        Array.Copy(f, 1, copies, 0, copies.Length);
                        return copies;
                    }
            }
            return new string[0];
        }

        // One Explorer window per folder, with its files selected. Returns the files that exist.
        public static int Show(string[] files)
        {
            int shown = 0;
            var byFolder = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            foreach (var f in files)
            {
                if (!File.Exists(f)) continue;
                string dir = Path.GetDirectoryName(f);
                List<string> list;
                if (!byFolder.TryGetValue(dir, out list)) byFolder[dir] = list = new List<string>();
                list.Add(f);
                shown++;
            }
            foreach (var kv in byFolder)
            {
                IntPtr folder = ILCreateFromPathW(kv.Key);
                if (folder == IntPtr.Zero) continue;
                var items = new List<IntPtr>();
                foreach (var f in kv.Value)
                {
                    IntPtr p = ILCreateFromPathW(f);
                    if (p != IntPtr.Zero) items.Add(p);
                }
                SHOpenFolderAndSelectItems(folder, (uint)items.Count, items.ToArray(), 0);
                foreach (var p in items) ILFree(p);
                ILFree(folder);
            }
            return shown;
        }
    }

    public static class CountedData
    {
        // Forgets the files of a folder tree in the index of counted locations: the index files of the
        // counts made inside it are deleted, and its files are dropped from the counts of folders that
        // contain it. Returns the number of entries forgotten.
        public static int ForgetIndex(string indexDir, string folder)
        {
            if (!Directory.Exists(indexDir)) return 0;
            int forgotten = 0;
            foreach (var file in Directory.GetFiles(indexDir, "*.tsv"))
            {
                // Only the counts inside the folder, or of a folder containing it, can have its files
                string root = GlobalIndex.RootOf(file);
                if (root == null || !(Paths.IsUnder(root, folder) || Paths.IsUnder(folder, root))) continue;
                var list = GlobalIndex.Load(file, out root);
                if (list == null) continue;
                if (Paths.IsUnder(root, folder))
                {
                    forgotten += list.Count;
                    File.Delete(file);
                    continue;
                }
                int before = list.Count;
                list.RemoveAll(e => Paths.IsUnder(e.Path, folder));
                if (list.Count == before) continue;
                forgotten += before - list.Count;
                GlobalIndex.Save(file, root, list);
            }
            return forgotten;
        }

        // The same for the copies of the files ("duplicated in"): its files, and its files as copies
        // of files elsewhere. Returns the number of files whose copies were forgotten.
        public static int ForgetCopies(string copiesDir, string folder)
        {
            if (!Directory.Exists(copiesDir)) return 0;
            int forgotten = 0;
            foreach (var file in Directory.GetFiles(copiesDir, "*.tsv"))
            {
                string root = GlobalIndex.RootOf(file);
                if (root == null) continue;
                if (Paths.IsUnder(root, folder)) { File.Delete(file); continue; }
                forgotten += GlobalIndex.FilterCopies(file, folder, true);
            }
            return forgotten;
        }
    }

    internal static class GlobalIndex
    {
        // The copies of every file of a count, for the "duplicated in" column of the files (read by
        // DicomShell.dll): one file per counted folder, "path<TAB>copy<TAB>copy...". Like the index,
        // this count replaces what other counts said about its own files.
        public static void SaveCopies(string dir, string root, Dictionary<string, List<string>> copies)
        {
            string own = FileFor(dir, root);
            if (Directory.Exists(dir))
                foreach (var file in Directory.GetFiles(dir, "*.tsv"))
                {
                    if (string.Equals(file, own, StringComparison.OrdinalIgnoreCase)) continue;
                    string other = RootOf(file);
                    if (other == null) continue;
                    if (Paths.IsUnder(other, root)) File.Delete(file);                       // counted again inside this one
                    else if (Paths.IsUnder(root, other)) FilterCopies(file, root, false);     // its lines for this folder
                }
            if (copies.Count == 0) { if (File.Exists(own)) File.Delete(own); return; }
            var lines = new List<string>();
            foreach (var kv in copies) lines.Add(kv.Key + "\t" + string.Join("\t", kv.Value));
            WriteCopies(dir, own, root, lines);
        }

        // Drops the lines of the files inside the folder; with references, also its files as copies of others
        public static int FilterCopies(string file, string folder, bool references)
        {
            string root = null;
            var kept = new List<string>();
            int dropped = 0;
            bool changed = false;
            foreach (var line in File.ReadAllLines(file, Encoding.UTF8))
            {
                if (line.StartsWith("#root\t")) { root = line.Substring(6); continue; }
                var f = line.Split('\t');
                if (f.Length < 2 || Paths.IsUnder(f[0], folder)) { dropped++; continue; }
                if (!references) { kept.Add(line); continue; }
                var others = new List<string>();
                for (int i = 1; i < f.Length; i++)
                {
                    string copy = f[i].EndsWith(" (differs)") ? f[i].Substring(0, f[i].Length - 10) : f[i];
                    if (!Paths.IsUnder(copy, folder)) others.Add(f[i]);
                }
                if (others.Count == 0) { dropped++; continue; }
                if (others.Count == f.Length - 1) kept.Add(line);
                else { kept.Add(f[0] + "\t" + string.Join("\t", others)); changed = true; }
            }
            if (root == null || (dropped == 0 && !changed)) return 0;
            if (kept.Count == 0) File.Delete(file);
            else WriteCopies(Path.GetDirectoryName(file), file, root, kept);
            return dropped;
        }

        static void WriteCopies(string dir, string file, string root, List<string> lines)
        {
            Directory.CreateDirectory(dir);
            string temp = file + ".tmp";
            using (var w = new StreamWriter(temp, false, new UTF8Encoding(false)))
            {
                w.WriteLine("#root\t" + root);
                foreach (var l in lines) w.WriteLine(l);
            }
            if (File.Exists(file)) File.Delete(file);
            File.Move(temp, file);
        }

        public static string RootOf(string file)
        {
            try
            {
                using (var r = new StreamReader(file, Encoding.UTF8))
                {
                    var f = (r.ReadLine() ?? "").Split('\t');
                    return f.Length == 2 && f[0] == "#root" ? f[1] : null;
                }
            }
            catch (Exception) { return null; }
        }

        public static string FileFor(string indexDir, string root)
        {
            using (var sha = System.Security.Cryptography.SHA1.Create())
            {
                var hash = sha.ComputeHash(Encoding.UTF8.GetBytes(root.TrimEnd('\\').ToLowerInvariant()));
                return Path.Combine(indexDir, BitConverter.ToString(hash).Replace("-", "").ToLowerInvariant() + ".tsv");
            }
        }

        public static void Save(string file, string root, IEnumerable<IndexEntry> entries)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(file));
            string temp = file + ".tmp";
            using (var w = new StreamWriter(temp, false, new UTF8Encoding(false)))
            {
                w.WriteLine("#root\t" + root);
                foreach (var e in entries)
                    w.WriteLine(e.Path + "\t" + e.Size + "\t" + e.Ticks + "\t" + e.Study + "\t" + e.Sop);
            }
            if (File.Exists(file)) File.Delete(file);
            File.Move(temp, file);
        }

        public static List<IndexEntry> Load(string file, out string root)
        {
            root = null;
            var list = new List<IndexEntry>();
            try
            {
                foreach (var line in File.ReadLines(file, Encoding.UTF8))
                {
                    var f = line.Split('\t');
                    if (f[0] == "#root") { root = f[1]; continue; }
                    if (f.Length < 5) continue;
                    list.Add(new IndexEntry { Path = f[0], Size = long.Parse(f[1]), Ticks = long.Parse(f[2]), Study = f[3], Sop = f[4] });
                }
            }
            catch (Exception) { return null; }
            return root == null ? null : list;
        }
    }

    internal class StudyAgg
    {
        readonly string uid;
        string date, patientId, description;
        readonly List<string> modalities = new List<string>();
        readonly Dictionary<string, int> modalityFiles = new Dictionary<string, int>();
        readonly HashSet<string> series = new HashSet<string>();
        int files;

        // Modalities that are not images (reports, presentation states, key objects, RT objects...)
        static readonly HashSet<string> NonImage = new HashSet<string> {
            "SR", "PR", "KO", "DOC", "REG", "SEG", "FID", "RWV", "PLAN", "AU",
            "RTSTRUCT", "RTPLAN", "RTDOSE", "RTRECORD", "RTIMAGE" };

        public StudyAgg(string uid) { this.uid = uid; }

        public string StudyDate { get { lock (this) return date; } }      // yyyyMMdd as stored in the DICOM

        // The image modality with most files (or the most frequent one if there are no images)
        public string MainModality()
        {
            lock (this)
            {
                string best = null, bestAny = null;
                int bestCount = 0, bestAnyCount = 0;
                foreach (var kv in modalityFiles)
                {
                    if (kv.Value > bestAnyCount) { bestAny = kv.Key; bestAnyCount = kv.Value; }
                    if (!NonImage.Contains(kv.Key) && kv.Value > bestCount) { best = kv.Key; bestCount = kv.Value; }
                }
                return best ?? bestAny;
            }
        }

        public void Add(DicomTags t)
        {
            lock (this)
            {
                files++;
                if (!string.IsNullOrEmpty(t.Modality))
                {
                    int n;
                    modalityFiles.TryGetValue(t.Modality, out n);
                    modalityFiles[t.Modality] = n + 1;
                }
                if (!string.IsNullOrEmpty(t.SeriesUid)) series.Add(t.SeriesUid);
                if (string.IsNullOrEmpty(date)) date = t.Date;
                if (string.IsNullOrEmpty(patientId)) patientId = t.PatientID;
                if (string.IsNullOrEmpty(description)) description = t.Description;
                if (!string.IsNullOrEmpty(t.Modality) && !modalities.Contains(t.Modality)) modalities.Add(t.Modality);
            }
        }

        public Study ToStudy()
        {
            modalities.Sort(StringComparer.Ordinal);
            string d = date;
            if (d != null && d.Length == 8) d = d.Substring(0, 4) + "-" + d.Substring(4, 2) + "-" + d.Substring(6, 2);
            return new Study
            {
                StudyInstanceUID = uid,
                Series = series.Count,
                StudyDate = d,
                PatientID = patientId,
                StudyDescription = description,
                Modalities = string.Join(",", modalities.ToArray()),
                Files = files
            };
        }
    }
}
