// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): paths, the comparison of files, and the Mark of the Web (unblocking)

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

namespace DicomStudyCount
{
    public static class Paths
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetLongPathNameW(string shortPath, StringBuilder longPath, uint size);

        // The long form of a path ("C:\Users\JOHNDO~1\x" -> "C:\Users\JohnDoe\x"): the same folder
        // reached through its 8.3 short name would otherwise be another location in the index (and its
        // files copies of themselves). The path as it is if Windows cannot tell.
        public static string Long(string path)
        {
            var sb = new StringBuilder(1024);
            uint n = GetLongPathNameW(path, sb, (uint)sb.Capacity);
            if (n > sb.Capacity) { sb = new StringBuilder((int)n); n = GetLongPathNameW(path, sb, (uint)sb.Capacity); }
            return n == 0 || n > sb.Capacity ? path : sb.ToString();
        }

        // The path is the folder itself or inside it
        public static bool IsUnder(string path, string folder)
        {
            path = path.TrimEnd('\\');
            folder = folder.TrimEnd('\\');
            return string.Equals(path, folder, StringComparison.OrdinalIgnoreCase) ||
                   path.StartsWith(folder + "\\", StringComparison.OrdinalIgnoreCase);
        }
    }

    // Byte-exact comparison: same size, then a full byte-by-byte comparison of the contents, so two
    // different files can never look identical. All the files of a group are read side by side in a
    // single pass: every byte is read once, and the comparison stops at the first difference.
    public static class FileCompare
    {
        const int BlockSize = 262144;
        const int Batch = 16;       // files compared at a time with the first one (open files, memory)

        [DllImport("msvcrt.dll", CallingConvention = CallingConvention.Cdecl)]
        static extern int memcmp(byte[] a, byte[] b, UIntPtr count);

        public static bool Identical(string a, string b)
        {
            return AllIdentical(new[] { a, b });
        }

        public static bool AllIdentical(IList<string> files)
        {
            try
            {
                long size = new FileInfo(files[0]).Length;
                for (int i = 1; i < files.Count; i++)
                    if (new FileInfo(files[i]).Length != size) return false;
                for (int start = 1; start < files.Count; start += Batch)
                {
                    int count = Math.Min(Batch, files.Count - start);
                    var streams = new FileStream[count + 1];
                    try
                    {
                        for (int i = 0; i <= count; i++)
                            streams[i] = new FileStream(i == 0 ? files[0] : files[start + i - 1], FileMode.Open,
                                                        FileAccess.Read, FileShare.Read, 4096, FileOptions.SequentialScan);
                        var first = new byte[BlockSize];
                        var other = new byte[BlockSize];
                        while (true)
                        {
                            int n = ReadFull(streams[0], first);
                            for (int i = 1; i <= count; i++)
                                if (ReadFull(streams[i], other) != n || memcmp(first, other, (UIntPtr)n) != 0) return false;
                            if (n == 0) break;
                        }
                    }
                    finally
                    {
                        foreach (var s in streams) if (s != null) s.Dispose();
                    }
                }
                return true;
            }
            catch (Exception) { return false; }
        }

        static int ReadFull(Stream s, byte[] buffer)
        {
            int total = 0;
            while (total < buffer.Length)
            {
                int n = s.Read(buffer, total, buffer.Length - total);
                if (n <= 0) break;
                total += n;
            }
            return total;
        }
    }

    // Index of counted locations: one tab-separated file per counted folder
    // ("#root <folder>" line, then path, size, last write time, StudyInstanceUID, SOPInstanceUID)
    // Files downloaded from the Internet carry a Zone.Identifier stream ("mark of the web"), and Windows
    // does not show them in the preview pane. Unblock removes that stream from the DICOM files of a
    // folder tree (as Unblock-File does), and only from them; their contents are not touched.
    public static class Unblocker
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool DeleteFileW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFileAttributesW(string path);

        // Returns { DICOM files unblocked, marked files left as they are (not DICOM), failures }
        public static int[] Run(string root, int threads)
        {
            int unblocked = 0, notDicom = 0, failed = 0;
            var files = new List<string>();
            var dirs = new Stack<string>();
            dirs.Push(root);
            while (dirs.Count > 0)
            {
                string d = dirs.Pop();
                try
                {
                    foreach (var f in Directory.EnumerateFiles(d)) files.Add(f);
                    foreach (var s in Directory.EnumerateDirectories(d)) dirs.Push(s);
                }
                catch (Exception) { }
            }
            var opts = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, threads) };
            Parallel.ForEach(files, opts, f =>
            {
                string mark = f + ":Zone.Identifier";
                if (GetFileAttributesW(mark) == 0xFFFFFFFF) return;     // not marked
                DicomTags tags = null;
                try { tags = DicomParser.ReadFile(f); } catch (Exception) { }
                // A DICOMDIR is not read as an image, but the preview shows it too: any file with "DICM"
                if (tags == null && !HasPreamble(f)) { Interlocked.Increment(ref notDicom); return; }
                if (DeleteFileW(mark)) Interlocked.Increment(ref unblocked);
                else Interlocked.Increment(ref failed);
            });
            return new[] { unblocked, notDicom, failed };
        }

        static bool HasPreamble(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 256))
                {
                    var head = new byte[132];
                    int n = 0, got;
                    while (n < head.Length && (got = fs.Read(head, n, head.Length - n)) > 0) n += got;
                    return n == 132 && head[128] == 'D' && head[129] == 'I' && head[130] == 'C' && head[131] == 'M';
                }
            }
            catch (Exception) { return false; }
        }
    }
}
