// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the saved views of the folders (the Bags of Explorer): the columns of the DICOM folders

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
    // Explorer remembers the view of every folder it has shown (Shell\Bags in the registry, found through
    // the item id lists of Shell\BagMRU), columns included: the Comments column added by a count stays
    // in the saved views of those folders. ColInfo, the saved columns (not documented; the same since
    // Windows 7): 16 zero bytes, signature FDDFDFFD, three DWORDs, column count, entry size (24), then
    // one entry per column: FMTID (16 bytes), PID, width. Sort: 16 zero bytes, count, then FMTID, PID
    // and direction per sort column.
    public static class ViewBags
    {
        const string ShellKey = @"Software\Classes\Local Settings\Software\Microsoft\Windows\Shell";
        static readonly byte[] CommentKey = PropertyKey(new Guid("F29F85E0-4FF9-1068-AB91-08002B27B3D9"), 6);
        static readonly byte[] NameKey = PropertyKey(new Guid("B725F130-47EF-101A-A5F1-02608C9EEBAC"), 10);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern bool SHGetPathFromIDListW(IntPtr pidl, StringBuilder path);

        static byte[] PropertyKey(Guid fmtid, int pid)
        {
            var key = new byte[20];
            Buffer.BlockCopy(fmtid.ToByteArray(), 0, key, 0, 16);
            Buffer.BlockCopy(BitConverter.GetBytes(pid), 0, key, 16, 4);
            return key;
        }

        // Removes the Comments column from the saved views of the folder (and of its whole subtree with
        // subtree = true). Returns the number of saved views changed.
        public static int RemoveCommentColumn(string folder, bool subtree)
        {
            folder = folder.TrimEnd('\\');
            int changed = 0;
            using (var mru = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(ShellKey + @"\BagMRU"))
            using (var bags = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(ShellKey + @"\Bags", true))
            {
                if (mru == null || bags == null) return 0;
                Walk(mru, new byte[0], folder, subtree, bags, ref changed, 0);
            }
            return changed;
        }

        // Every node of BagMRU is one item (its id in the value of the parent, its children in the
        // subkey of the same name): the branches that cannot lead to the folder are skipped
        static void Walk(Microsoft.Win32.RegistryKey key, byte[] prefix, string folder, bool subtree,
                         Microsoft.Win32.RegistryKey bags, ref int changed, int depth)
        {
            if (depth > 60) return;
            foreach (var name in key.GetValueNames())
            {
                int index;
                if (!int.TryParse(name, out index)) continue;
                var item = key.GetValue(name) as byte[];
                if (item == null || item.Length < 2) continue;
                int cb = BitConverter.ToUInt16(item, 0);
                if (cb < 2 || cb > item.Length) continue;
                var ids = new byte[prefix.Length + cb];
                Buffer.BlockCopy(prefix, 0, ids, 0, prefix.Length);
                Buffer.BlockCopy(item, 0, ids, prefix.Length, cb);

                string path = PathOf(ids);
                bool match = false, descend = true;
                if (!string.IsNullOrEmpty(path))
                {
                    path = path.TrimEnd('\\');
                    bool same = string.Equals(path, folder, StringComparison.OrdinalIgnoreCase);
                    bool inside = Paths.IsUnder(path, folder);
                    match = same || (subtree && inside);
                    // Below the folder (or a folder outside it): nothing else to find in this branch
                    descend = Paths.IsUnder(folder, path) || (subtree && (same || inside));
                }
                if (!match && !descend) continue;
                using (var sub = key.OpenSubKey(name))
                {
                    if (sub == null) continue;
                    if (match)
                    {
                        var slot = sub.GetValue("NodeSlot");
                        if (slot is int) changed += CleanBag(bags, ((int)slot).ToString());
                    }
                    if (descend) Walk(sub, ids, folder, subtree, bags, ref changed, depth + 1);
                }
            }
        }

        static string PathOf(byte[] ids)
        {
            IntPtr p = Marshal.AllocCoTaskMem(ids.Length + 2);
            try
            {
                Marshal.Copy(ids, 0, p, ids.Length);
                Marshal.WriteInt16(p, ids.Length, 0);       // terminator of the item id list
                var sb = new StringBuilder(1024);
                return SHGetPathFromIDListW(p, sb) ? sb.ToString() : null;
            }
            finally { Marshal.FreeCoTaskMem(p); }
        }

        // One saved view can hold several layouts (one per folder type, file dialogs...)
        static int CleanBag(Microsoft.Win32.RegistryKey bags, string slot)
        {
            using (var bag = bags.OpenSubKey(slot, true))
                return bag == null ? 0 : CleanLayouts(bag);
        }

        static int CleanLayouts(Microsoft.Win32.RegistryKey key)
        {
            int changed = 0;
            bool any = false;
            var colInfo = key.GetValue("ColInfo") as byte[];
            var cols = colInfo == null || colInfo.Length < 0x28 ? null : Without(colInfo, 0x20, 0x28, BitConverter.ToInt32(colInfo, 0x24), 20);
            if (cols != null) { key.SetValue("ColInfo", cols, Microsoft.Win32.RegistryValueKind.Binary); any = true; }

            var sort = key.GetValue("Sort") as byte[];
            var sorted = sort == null || sort.Length < 0x14 ? null : Without(sort, 0x10, 0x14, 24, 20);
            if (sorted != null)
            {
                // Sorted by comment only: sorted by name, the default
                if (BitConverter.ToInt32(sorted, 0x10) == 0)
                {
                    var byName = new byte[0x14 + 24];
                    Buffer.BlockCopy(sorted, 0, byName, 0, 0x10);
                    byName[0x10] = 1;
                    Buffer.BlockCopy(NameKey, 0, byName, 0x14, 20);
                    byName[0x14 + 20] = 1;              // ascending
                    sorted = byName;
                }
                key.SetValue("Sort", sorted, Microsoft.Win32.RegistryValueKind.Binary);
                any = true;
            }

            // Grouped by comment: not grouped
            var groupFmtid = key.GetValue("GroupByKey:FMTID") as string;
            var groupPid = key.GetValue("GroupByKey:PID");
            if (groupFmtid != null && groupPid is int && (int)groupPid == 6 &&
                string.Equals(groupFmtid.Trim('{', '}'), "F29F85E0-4FF9-1068-AB91-08002B27B3D9", StringComparison.OrdinalIgnoreCase))
            {
                key.DeleteValue("GroupByKey:FMTID", false);
                key.DeleteValue("GroupByKey:PID", false);
                key.DeleteValue("GroupByDirection", false);
                if (key.GetValue("GroupView") != null) key.SetValue("GroupView", 0, Microsoft.Win32.RegistryValueKind.DWord);
                any = true;
            }
            if (any) changed++;

            foreach (var name in key.GetSubKeyNames())
                using (var sub = key.OpenSubKey(name, true))
                    if (sub != null) changed += CleanLayouts(sub);
            return changed;
        }

        // The list without the entries of the Comments property: count at countAt, entries from
        // first on, keyed by their first keyLength bytes. null if there was none (or not this layout).
        static byte[] Without(byte[] data, int countAt, int first, int entrySize, int keyLength)
        {
            if (data.Length < first || countAt + 4 > data.Length || entrySize < keyLength) return null;
            if (first == 0x28 && BitConverter.ToUInt32(data, 0x10) != 0xFDDFDFFD) return null;
            int count = BitConverter.ToInt32(data, countAt);
            if (count < 0 || first + (long)count * entrySize > data.Length) return null;
            var kept = new List<byte>(data.Length);
            for (int i = 0; i < first; i++) kept.Add(data[i]);
            int left = 0;
            for (int e = 0; e < count; e++)
            {
                int at = first + e * entrySize;
                bool comment = true;
                for (int k = 0; k < keyLength && comment; k++) comment = data[at + k] == CommentKey[k];
                if (comment) continue;
                for (int k = 0; k < entrySize; k++) kept.Add(data[at + k]);
                left++;
            }
            if (left == count) return null;
            for (int i = first + count * entrySize; i < data.Length; i++) kept.Add(data[i]);
            var result = kept.ToArray();
            Buffer.BlockCopy(BitConverter.GetBytes(left), 0, result, countAt, 4);
            return result;
        }
    }
}
