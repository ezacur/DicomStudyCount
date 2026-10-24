// DicomStudyCount: the C# part of Count-DicomStudies.ps1 (DICOM header reader, count, folder comments
// and icons, Explorer columns and saved views, index of counted locations). Built into
// bin\DicomStudyCount.dll by tools\Build.ps1 (.NET Framework 4, for Windows PowerShell 5.1).

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
    public class Study
    {
        public string StudyDate { get; set; }
        public string Modalities { get; set; }
        public string PatientID { get; set; }
        public string StudyDescription { get; set; }
        public int Series { get; set; }
        public int Files { get; set; }
        public string StudyInstanceUID { get; set; }
    }

    public class ScanProgress
    {
        public long TotalFiles;
        public long Files;      // files processed
        public long Dicom;      // DICOM with StudyInstanceUID
        public long NoStudy;    // DICOM without StudyInstanceUID
        public long NotDicom;
        public long Errors;
        public int Studies;
        public long CommentErrors;
        public volatile bool CountingShown;     // "Counting..." already written and visible in Explorer
        public List<string> InaccessibleDirs = new List<string>();
    }

    public class FolderStudies
    {
        public string Path { get; set; }
        public int Studies { get; set; }
        public int Series { get; set; }
        public int Files { get; set; }
        public string StudyInstanceUID { get; set; }   // only when the folder holds a single study
    }

    // A study found in more than one separate folder (its topmost single-study folders)
    public class StudySplit
    {
        public string Tag { get; set; }
        public string StudyInstanceUID { get; set; }
        public List<string> Folders { get; set; }
    }

    // Files sharing one SOPInstanceUID: identical copies (same bytes) or re-processed versions
    public class DuplicateGroup
    {
        public bool Identical { get; set; }
        public List<string> Files { get; set; }
    }

    // Files with the same SOPInstanceUID (the same image) found in several folders,
    // grouped by the set of folders involved
    public class DuplicateSet
    {
        public int Files { get; set; }
        public int Identical { get; set; }      // files in groups of identical copies
        public int Different { get; set; }      // files in groups whose contents differ
        public List<string> Folders { get; set; }
        public string Keep { get; set; }        // suggested folder to keep the copies in
        public List<DuplicateGroup> Groups { get; set; }
    }

    public class ScanResult
    {
        public ScanProgress Counts;
        public List<Study> Studies;
        public List<FolderStudies> Folders;
        public List<StudySplit> Splits;
        public List<DuplicateSet> Duplicates;
        public int DuplicatedFiles;      // files that have a copy elsewhere (all copies counted)
        public List<IncompleteSeries> Incomplete;
        public List<ExternalCopySet> External;
    }

    // A series that, in this folder, lacks some of the InstanceNumbers seen for it in the whole tree
    public class IncompleteSeries
    {
        public string Tag { get; set; }         // study tag
        public string Series { get; set; }      // "<SeriesNumber>.<SeriesDescription>"
        public string Folder { get; set; }
        public int Have { get; set; }
        public int Expected { get; set; }
        public string Missing { get; set; }     // missing InstanceNumbers, as ranges ("4, 7-9, ...")
    }

    // Byte-identical copies (or same study + SOPInstanceUID with different contents) found in another
    // counted location, grouped by this folder and the other folder
    public class ExternalCopySet
    {
        public int Files { get; set; }
        public int Identical { get; set; }
        public int Different { get; set; }
        public string Folder { get; set; }
        public string OtherFolder { get; set; }
    }

    // Folder comments via desktop.ini. Explorer shows the InfoTip in the Comments column, but it
    // only reads desktop.ini when the folder has the Read-only or System attribute.
    // The [DicomStudyCount] section marks what this script wrote, so comments or settings
    // made by anyone else are never removed.
    public static class FolderComments
    {
        public const string CountingText = "Counting StudyUIDs...";
        public const string EmptyText = "---";

        // Files but no DICOM in the whole subtree: "(0 DICOMs of 12 files)"
        public static string NoDicomText(int totalFiles)
        {
            return "(0 DICOMs of " + totalFiles + (totalFiles == 1 ? " file)" : " files)");
        }
        const string Marker = "DicomStudyCount";

        // One lock per folder (striped by path), not one for all: different folders are written in
        // parallel, which is several times faster (the shell API takes ~7 ms per folder)
        static readonly object[] locks = CreateLocks(256);

        static object[] CreateLocks(int n)
        {
            var l = new object[n];
            for (int i = 0; i < n; i++) l[i] = new object();
            return l;
        }

        static object LockFor(string dir)
        {
            int h = StringComparer.OrdinalIgnoreCase.GetHashCode(dir.TrimEnd('\\'));
            return locks[(h & 0x7FFFFFFF) % locks.Length];
        }

        public const char Warning = '⚠';

        const string Base62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        // Short tag that identifies a study: its main modality and 6 case-sensitive characters (62
        // possible each, 56.8e9 tags) from a 64-bit FNV-1a hash of the StudyInstanceUID, always the
        // same for the same study: "CT-aB3x9Q"
        public static string StudyTag(string uid, string modality)
        {
            ulong h = 14695981039346656037;
            foreach (char c in uid) { h ^= c; h *= 1099511628211; }
            ulong v = h % 56800235584UL;    // 62^6
            var chars = new char[6];
            for (int i = 5; i >= 0; i--) { chars[i] = Base62[(int)(v % 62)]; v /= 62; }
            return (string.IsNullOrEmpty(modality) ? "OT" : modality) + "-" + new string(chars);
        }

        // StudyDate (yyyyMMdd) as dd/MMM/yyyy, with English month names: "26/Mar/2026"
        public static string FormatDate(string dicomDate)
        {
            DateTime d;
            if (string.IsNullOrEmpty(dicomDate)) return "no date";
            if (DateTime.TryParseExact(dicomDate, "yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture,
                                       System.Globalization.DateTimeStyles.None, out d))
                return d.ToString("dd/MMM/yyyy", System.Globalization.CultureInfo.InvariantCulture);
            return dicomDate;
        }

        // Several studies -> "N DICOM studies"; one -> "[CT-xxxxx] dd/MMM/yyyy - N series"; one study with
        // a single series -> "[CT-xxxxx] dd/MMM/yyyy - <SeriesNumber %03d>.<SeriesDescription> (<DICOM files> of <files>)"
        public static string Format(int studies, string tag, string studyDate, int series, string seriesNumber,
                                    string seriesDescription, int dicomFiles, int totalFiles)
        {
            if (studies != 1) return studies + " DICOM studies";
            string prefix = "[" + tag + "] " + FormatDate(studyDate);
            if (series == 0) return prefix;
            if (series > 1) return prefix + " - " + series + " series";

            int number;
            string label = int.TryParse(seriesNumber, out number) ? number.ToString("D3") : (seriesNumber ?? "");
            if (!string.IsNullOrEmpty(seriesDescription))
                label = label == "" ? seriesDescription : label + "." + seriesDescription;
            if (label == "") label = "1 series";
            return prefix + " - " + label + " (" + dicomFiles + " of " + totalFiles + ")";
        }

        const FileAttributes FolderCustomAttrs = FileAttributes.ReadOnly | FileAttributes.System;

        // A custom icon set by someone else (IconResource, or the older IconFile) is never replaced
        static bool HasForeignIcon(string ini)
        {
            if (!File.Exists(ini)) return false;
            string icon = Native.ReadIni(ini, ".ShellClassInfo", "IconResource");
            if (icon != "" && !FolderIcons.IsOurs(FolderIcons.PathOf(icon))) return true;
            return Native.ReadIni(ini, ".ShellClassInfo", "IconFile") != "";
        }

        // icon: full path of the folder icon, "" to remove the icon of this script, null to leave it as it is
        public static void Set(string dir, string text, string icon = null)
        {
            string ini = Path.Combine(dir, "desktop.ini");
            lock (LockFor(dir))
            {
                var before = File.GetAttributes(dir);
                bool hadMarker = File.Exists(ini) && Native.ReadIni(ini, Marker, "Generated") != "";
                if (icon != null && HasForeignIcon(ini)) icon = null;
                if (File.Exists(ini))
                {
                    // Drop the Unicode copy of the previous text: the API only rewrites it for non-ASCII text
                    File.SetAttributes(ini, FileAttributes.Normal);
                    Native.DeleteIniKey(ini, ".ShellClassInfo.W", "InfoTip");
                    Native.DeleteIniKey(ini, ".ShellClassInfo.A", "InfoTip");
                    if (icon == "") Native.DeleteIniKey(ini, ".ShellClassInfo", "IconResource");
                }

                // Using the official API (rather than writing the .ini by hand) is what makes
                // Explorer invalidate its desktop.ini cache and refresh the column right away
                Native.SetFolderInfoTip(dir, text, icon == "" ? null : icon);

                File.SetAttributes(ini, FileAttributes.Normal);
                Native.WritePrivateProfileString(Marker, "Generated", "1", ini);
                // "General items" folder type, so all folders share the same column template
                // (the one set with "Apply to Folders"). Only if no type was set already.
                if (Native.ReadIni(ini, "ViewState", "FolderType") == "")
                {
                    Native.WritePrivateProfileString("ViewState", "FolderType", "Generic", ini);
                    Native.WritePrivateProfileString(Marker, "SetFolderType", "1", ini);
                }
                // Folder attributes added by us (or by the API), so they can be removed later
                var after = File.GetAttributes(dir);
                if ((after & FolderCustomAttrs) == 0)
                {
                    after |= FileAttributes.ReadOnly;
                    File.SetAttributes(dir, after);
                }
                if (!hadMarker)
                {
                    int added = (int)(after & ~before & FolderCustomAttrs);
                    if (added != 0) Native.WritePrivateProfileString(Marker, "AddedAttributes", added.ToString(), ini);
                }
                Native.FlushIni(ini);
                File.SetAttributes(ini, FileAttributes.Hidden | FileAttributes.System);
            }
            Native.NotifyUpdated(dir);
        }

        // Removes only what this script wrote; deletes desktop.ini if nothing else is left in it
        public static bool Clear(string dir)
        {
            string ini = Path.Combine(dir, "desktop.ini");
            lock (LockFor(dir))
            {
                if (!File.Exists(ini) || Native.ReadIni(ini, Marker, "Generated") == "") return false;
                File.SetAttributes(ini, FileAttributes.Normal);
                if (FolderIcons.IsOurs(FolderIcons.PathOf(Native.ReadIni(ini, ".ShellClassInfo", "IconResource"))))
                    Native.DeleteIniKey(ini, ".ShellClassInfo", "IconResource");
                Native.SetFolderInfoTip(dir, "");       // clears the column in Explorer
                File.SetAttributes(ini, FileAttributes.Normal);
                Native.DeleteIniKey(ini, ".ShellClassInfo", "InfoTip");
                // With non-ASCII characters the API also stores a copy in .ShellClassInfo.W / .A
                Native.DeleteIniKey(ini, ".ShellClassInfo.W", "InfoTip");
                Native.DeleteIniKey(ini, ".ShellClassInfo.A", "InfoTip");
                if (Native.ReadIni(ini, Marker, "SetFolderType") != "") Native.DeleteIniKey(ini, "ViewState", "FolderType");
                int added;
                int.TryParse(Native.ReadIni(ini, Marker, "AddedAttributes"), out added);
                if (Native.ReadIni(ini, Marker, "SetReadOnly") != "") added |= (int)FileAttributes.ReadOnly;   // older versions
                Native.DeleteIniSection(ini, Marker);
                Native.FlushIni(ini);
                string rest = Regex.Replace(File.ReadAllText(ini), @"\[(\.ShellClassInfo(\.[AW])?|ViewState)\]", "");
                if (rest.Trim().Length == 0)
                {
                    File.Delete(ini);
                    if (added != 0) File.SetAttributes(dir, File.GetAttributes(dir) & ~(FileAttributes)added);
                }
                else File.SetAttributes(ini, FileAttributes.Hidden | FileAttributes.System);
            }
            Native.NotifyUpdated(dir);
            return true;
        }
    }

    // Adds the Comments column to an Explorer window that is already open
    // (ShellWindows -> IShellBrowser -> IShellView -> IColumnManager)
    public static class ExplorerColumns
    {
        [ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IComServiceProvider
        {
            void QueryService(ref Guid guidService, ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out object ppv);
        }

        [ComImport, Guid("000214E2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellBrowser
        {
            void GetWindow(out IntPtr phwnd);
            void ContextSensitiveHelp(bool fEnterMode);
            void InsertMenusSB(IntPtr hmenuShared, IntPtr lpMenuWidths);
            void SetMenuSB(IntPtr hmenuShared, IntPtr holemenuRes, IntPtr hwndActiveObject);
            void RemoveMenusSB(IntPtr hmenuShared);
            void SetStatusTextSB(IntPtr pszStatusText);
            void EnableModelessSB(bool fEnable);
            void TranslateAcceleratorSB(IntPtr pmsg, ushort wID);
            void BrowseObject(IntPtr pidl, uint wFlags);
            void GetViewStateStream(uint grfMode, out IntPtr ppStrm);
            void GetControlWindow(uint id, out IntPtr phwnd);
            void SendControlMsg(uint id, uint uMsg, IntPtr wParam, IntPtr lParam, out IntPtr pret);
            void QueryActiveShellView([MarshalAs(UnmanagedType.Interface)] out object ppshv);
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PROPERTYKEY { public Guid fmtid; public uint pid; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct CM_COLUMNINFO
        {
            public uint cbSize, dwMask, dwState, uWidth, uDefaultWidth, uIdealWidth;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string wszName;
        }

        [ComImport, Guid("D8EC27BB-3F3B-4042-B10A-4ACFD924D453"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IColumnManager
        {
            void SetColumnInfo(ref PROPERTYKEY propkey, ref CM_COLUMNINFO pcmci);
            void GetColumnInfo(ref PROPERTYKEY propkey, ref CM_COLUMNINFO pcmci);
            void GetColumnCount(uint dwFlags, out uint puCount);
            void GetColumns(uint dwFlags, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] PROPERTYKEY[] rgkeyOrder, uint cColumns);
            void SetColumns([In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] PROPERTYKEY[] rgkeyOrder, uint cVisible);
        }

        static readonly Guid SID_STopLevelBrowser = new Guid("4C96BE40-915C-11CF-99D3-00AA004AE837");
        static readonly Guid PKEY_Comment_fmtid = new Guid("F29F85E0-4FF9-1068-AB91-08002B27B3D9");
        const uint PKEY_Comment_pid = 6;
        const uint CM_ENUM_VISIBLE = 0x2;
        const uint CM_MASK_WIDTH = 0x1;
        const uint CM_WIDTH_AUTOSIZE = 0xFFFFFFFE;     // -2: width fitted to the content

        // Adds the Comments column as the last column if missing and fits its width to the text.
        // Returns true if it was added.
        public static bool ShowCommentColumn(object explorerWindow)
        {
            Guid sid = SID_STopLevelBrowser, iid = typeof(IShellBrowser).GUID;
            object browser;
            ((IComServiceProvider)explorerWindow).QueryService(ref sid, ref iid, out browser);
            object view;
            ((IShellBrowser)browser).QueryActiveShellView(out view);
            var cm = (IColumnManager)view;

            uint count;
            cm.GetColumnCount(CM_ENUM_VISIBLE, out count);
            var keys = new PROPERTYKEY[count];
            cm.GetColumns(CM_ENUM_VISIBLE, keys, count);
            var comment = new PROPERTYKEY { fmtid = PKEY_Comment_fmtid, pid = PKEY_Comment_pid };
            bool present = false;
            foreach (var k in keys)
                if (k.fmtid == PKEY_Comment_fmtid && k.pid == PKEY_Comment_pid) present = true;

            if (!present)
            {
                var list = new List<PROPERTYKEY>(keys);
                list.Add(comment);     // as the last column
                cm.SetColumns(list.ToArray(), (uint)list.Count);
            }

            var info = new CM_COLUMNINFO
            {
                cbSize = (uint)Marshal.SizeOf(typeof(CM_COLUMNINFO)),
                dwMask = CM_MASK_WIDTH,
                uWidth = CM_WIDTH_AUTOSIZE,
                wszName = ""
            };
            cm.SetColumnInfo(ref comment, ref info);
            return !present;
        }

        [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
        static extern int PSGetPropertyKeyFromName(string name, out PROPERTYKEY key);

        static IColumnManager ColumnsOf(object explorerWindow)
        {
            Guid sid = SID_STopLevelBrowser, iid = typeof(IShellBrowser).GUID;
            object browser;
            ((IComServiceProvider)explorerWindow).QueryService(ref sid, ref iid, out browser);
            object view;
            ((IShellBrowser)browser).QueryActiveShellView(out view);
            return (IColumnManager)view;
        }

        static List<PROPERTYKEY> Visible(IColumnManager cm)
        {
            uint count;
            cm.GetColumnCount(CM_ENUM_VISIBLE, out count);
            var keys = new PROPERTYKEY[count];
            cm.GetColumns(CM_ENUM_VISIBLE, keys, count);
            return new List<PROPERTYKEY>(keys);
        }

        // The keys of the properties named (those not registered are left out)
        static List<PROPERTYKEY> KeysOf(string[] names)
        {
            var keys = new List<PROPERTYKEY>();
            foreach (var n in names)
            {
                PROPERTYKEY k;
                if (PSGetPropertyKeyFromName(n, out k) == 0) keys.Add(k);
            }
            return keys;
        }

        static bool Contains(List<PROPERTYKEY> keys, PROPERTYKEY k)
        {
            foreach (var x in keys) if (x.fmtid == k.fmtid && x.pid == k.pid) return true;
            return false;
        }

        // True if any of the columns named is visible in the window
        public static bool AnyColumnShown(object explorerWindow, string[] names)
        {
            var visible = Visible(ColumnsOf(explorerWindow));
            foreach (var k in KeysOf(names)) if (Contains(visible, k)) return true;
            return false;
        }

        // Shows the columns named (after the visible ones, in that order, fitted to their content) or
        // hides them; the other columns stay as they are
        public static void SetColumnsShown(object explorerWindow, string[] names, bool show)
        {
            var cm = ColumnsOf(explorerWindow);
            var wanted = KeysOf(names);
            var columns = new List<PROPERTYKEY>();
            foreach (var k in Visible(cm)) if (!Contains(wanted, k)) columns.Add(k);
            if (show) columns.AddRange(wanted);
            cm.SetColumns(columns.ToArray(), (uint)columns.Count);
            if (!show) return;
            foreach (var k in wanted)
            {
                var key = k;
                var info = new CM_COLUMNINFO
                {
                    cbSize = (uint)Marshal.SizeOf(typeof(CM_COLUMNINFO)),
                    dwMask = CM_MASK_WIDTH,
                    uWidth = CM_WIDTH_AUTOSIZE,
                    wszName = ""
                };
                cm.SetColumnInfo(ref key, ref info);
            }
        }

        // Removes the Comments column from the view. Returns true if it was visible.
        public static bool HideCommentColumn(object explorerWindow)
        {
            Guid sid = SID_STopLevelBrowser, iid = typeof(IShellBrowser).GUID;
            object browser;
            ((IComServiceProvider)explorerWindow).QueryService(ref sid, ref iid, out browser);
            object view;
            ((IShellBrowser)browser).QueryActiveShellView(out view);
            var cm = (IColumnManager)view;

            uint count;
            cm.GetColumnCount(CM_ENUM_VISIBLE, out count);
            var keys = new PROPERTYKEY[count];
            cm.GetColumns(CM_ENUM_VISIBLE, keys, count);
            var list = new List<PROPERTYKEY>();
            foreach (var k in keys)
                if (!(k.fmtid == PKEY_Comment_fmtid && k.pid == PKEY_Comment_pid)) list.Add(k);
            if (list.Count == keys.Length) return false;
            cm.SetColumns(list.ToArray(), (uint)list.Count);
            return true;
        }
    }

    public static class Native
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool WritePrivateProfileString(string section, string key, string value, string file);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern uint GetPrivateProfileString(string section, string key, string def, StringBuilder ret, uint size, string file);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        public static extern void SHChangeNotify(int eventId, uint flags, string item1, IntPtr item2);

        [DllImport("user32.dll")]
        public static extern bool SetProcessDPIAware();

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct SHFOLDERCUSTOMSETTINGS
        {
            public uint dwSize, dwMask;
            public IntPtr pvid;
            public string pszWebViewTemplate;
            public uint cchWebViewTemplate;
            public string pszWebViewTemplateVersion;
            public string pszInfoTip;
            public uint cchInfoTip;
            public IntPtr pclsid;
            public uint dwFlags;
            public string pszIconFile;
            public uint cchIconFile;
            public int iIconIndex;
            public string pszLogo;
            public uint cchLogo;
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern int SHGetSetFolderCustomSettings(ref SHFOLDERCUSTOMSETTINGS pfcs, string pszPath, uint dwReadWrite);

        // iconFile: null leaves the folder icon as it is
        public static void SetFolderInfoTip(string dir, string text, string iconFile = null)
        {
            var s = new SHFOLDERCUSTOMSETTINGS
            {
                dwSize = (uint)Marshal.SizeOf(typeof(SHFOLDERCUSTOMSETTINGS)),
                dwMask = 0x4,           // FCSM_INFOTIP
                pszInfoTip = text
            };
            if (iconFile != null)
            {
                s.dwMask |= 0x10;       // FCSM_ICONFILE
                s.pszIconFile = iconFile;
                s.iIconIndex = 0;
            }
            int hr = SHGetSetFolderCustomSettings(ref s, dir, 0x2 /* FCS_FORCEWRITE */);
            if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        }

        public static string ReadIni(string file, string section, string key)
        {
            var sb = new StringBuilder(1024);
            GetPrivateProfileString(section, key, "", sb, (uint)sb.Capacity, file);
            return sb.ToString();
        }

        // PowerShell passes $null to P/Invoke as "", so calls that need a real null go through here
        public static bool WriteIni(string file, string section, string key, string value)
        {
            return WritePrivateProfileString(section, key, value, file);
        }

        public static void DeleteIniKey(string file, string section, string key)
        {
            WritePrivateProfileString(section, key, null, file);
        }

        public static void DeleteIniSection(string file, string section)
        {
            WritePrivateProfileString(section, null, null, file);
        }

        // Flushes to disk the cache Windows keeps for .ini files
        public static void FlushIni(string file)
        {
            WritePrivateProfileString(null, null, null, file);
        }

        // Tells Explorer the folder changed so it refreshes its columns
        public static void NotifyUpdated(string path)
        {
            SHChangeNotify(0x00002000 /* SHCNE_UPDATEITEM */, 0x0005 /* SHCNF_PATHW */, path, IntPtr.Zero);
        }
    }

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

    public static class Paths
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetLongPathNameW(string shortPath, StringBuilder longPath, uint size);

        // The long form of a path ("C:\Users\ERNEST~1\x" -> "C:\Users\ErnestoCorify\x"): the same folder
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

    internal class DicomTags
    {
        public string Uid, SeriesUid, SeriesNumber, InstanceNumber, SopUid, Date, Modality, PatientID, Description, SeriesDescription;
    }

    internal static class DicomParser
    {
        const uint TagMediaStorageSOPClass = 0x00020002;
        const uint TagTransferSyntax = 0x00020010;
        const uint TagDirectoryRecordSequence = 0x00041220;
        const uint TagSOPClassUID = 0x00080016;
        const uint TagSOPInstanceUID = 0x00080018;
        const uint TagStudyDate = 0x00080020;
        const uint TagModality = 0x00080060;
        const uint TagStudyDescription = 0x00081030;
        const uint TagSeriesDescription = 0x0008103E;
        const uint TagPatientID = 0x00100020;
        const uint TagStudyInstanceUID = 0x0020000D;
        const uint TagSeriesInstanceUID = 0x0020000E;
        const uint TagSeriesNumber = 0x00200011;
        const uint TagInstanceNumber = 0x00200013;

        // SOP Class of a DICOMDIR (Media Storage Directory Storage)
        const string DirectorySopClass = "1.2.840.10008.1.3.10";

        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        // null = not DICOM. Throws if it is DICOM but cannot be read. A DICOMDIR (the index of a
        // DICOM media, recognized by its contents, not by its name) is not counted either: null too.
        public static DicomTags ReadFile(string path)
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 4096))
            {
                var head = new byte[132];
                int n = ReadUpTo(fs, head);
                if (n == 132 && head[128] == 'D' && head[129] == 'I' && head[130] == 'C' && head[131] == 'M')
                {
                    fs.Position = 132;
                    return ReadAfterPreamble(fs);
                }

                // "Raw" DICOM without preamble: only attempted if it starts with group 0002 or 0008
                if (n < 8 || head[1] != 0 || (head[0] != 0x02 && head[0] != 0x08)) return null;
                fs.Position = 0;
                try
                {
                    var t = ReadAfterPreamble(fs);
                    return t == null || string.IsNullOrEmpty(t.Uid) ? null : t;
                }
                catch (Exception) { return null; }
            }
        }

        static DicomTags ReadAfterPreamble(FileStream fs)
        {
            // File meta information (group 0002): always Explicit VR Little Endian
            var r = new DicomReader(fs) { Explicit = true };
            string ts = null, mediaClass = null;
            while (fs.Position + 8 <= fs.Length)
            {
                long pos = fs.Position;
                uint tag = r.ReadTag();
                if ((tag >> 16) != 0x0002) { fs.Position = pos; break; }
                string vr;
                uint len = r.ReadLength(tag, out vr);
                if (tag == TagTransferSyntax) ts = Str(r.Bytes(len));
                else if (tag == TagMediaStorageSOPClass) mediaClass = Str(r.Bytes(len));
                else r.Skip(len);
            }
            if (mediaClass == DirectorySopClass) return null;   // DICOMDIR: its records are not read

            switch (ts)
            {
                case "1.2.840.10008.1.2":       // Implicit VR Little Endian
                    r.Explicit = false;
                    break;
                case "1.2.840.10008.1.2.2":     // Explicit VR Big Endian
                    r.BigEndian = true;
                    break;
                case "1.2.840.10008.1.2.1.99":  // Deflated Explicit VR Little Endian
                    r = new DicomReader(new DeflateStream(fs, CompressionMode.Decompress, true)) { Explicit = true };
                    break;
                case null:                      // no meta information: guess by looking for a VR
                    r.Explicit = LooksExplicit(fs);
                    break;
            }
            return ReadDataset(r);
        }

        static DicomTags ReadDataset(DicomReader r)
        {
            var t = new DicomTags();
            while (!r.AtEnd)
            {
                uint tag = r.ReadTag();
                if (tag > TagInstanceNumber) break;   // tags are sorted: they will not appear anymore
                string vr;
                uint len = r.ReadLength(tag, out vr);
                // DICOMDIR without (or with a wrong) meta information: it has directory records
                if (tag == TagDirectoryRecordSequence) return null;
                if (len == 0xFFFFFFFF) { r.SkipSequence(vr); continue; }
                switch (tag)
                {
                    case TagSOPClassUID:
                        if (Str(r.Bytes(len)) == DirectorySopClass) return null;
                        break;
                    case TagSOPInstanceUID: t.SopUid = Str(r.Bytes(len)); break;
                    case TagStudyDate: t.Date = Str(r.Bytes(len)); break;
                    case TagModality: t.Modality = Str(r.Bytes(len)); break;
                    case TagStudyDescription: t.Description = Str(r.Bytes(len)); break;
                    case TagSeriesDescription: t.SeriesDescription = Str(r.Bytes(len)); break;
                    case TagPatientID: t.PatientID = Str(r.Bytes(len)); break;
                    case TagStudyInstanceUID: t.Uid = Str(r.Bytes(len)); break;
                    case TagSeriesInstanceUID: t.SeriesUid = Str(r.Bytes(len)); break;
                    case TagSeriesNumber: t.SeriesNumber = Str(r.Bytes(len)); break;
                    case TagInstanceNumber: t.InstanceNumber = Str(r.Bytes(len)); return t;
                    default: r.Skip(len); break;
                }
            }
            return t;
        }

        static bool LooksExplicit(FileStream fs)
        {
            long pos = fs.Position;
            var b = new byte[6];
            int n = ReadUpTo(fs, b);
            fs.Position = pos;
            return n == 6 && b[4] >= 'A' && b[4] <= 'Z' && b[5] >= 'A' && b[5] <= 'Z';
        }

        static int ReadUpTo(Stream s, byte[] b)
        {
            int off = 0;
            while (off < b.Length)
            {
                int r = s.Read(b, off, b.Length - off);
                if (r <= 0) break;
                off += r;
            }
            return off;
        }

        static string Str(byte[] b)
        {
            return Latin1.GetString(b).Trim('\0', ' ');
        }
    }

    internal sealed class DicomReader
    {
        readonly Stream s;
        readonly byte[] buf = new byte[8];
        public bool Explicit;
        public bool BigEndian;

        public DicomReader(Stream s) { this.s = s; }

        public bool AtEnd { get { return s.CanSeek && s.Position + 4 > s.Length; } }

        void ReadExact(byte[] b, int n)
        {
            int off = 0;
            while (off < n)
            {
                int r = s.Read(b, off, n - off);
                if (r <= 0) throw new EndOfStreamException();
                off += r;
            }
        }

        public uint U16()
        {
            ReadExact(buf, 2);
            return BigEndian ? (uint)((buf[0] << 8) | buf[1]) : (uint)(buf[0] | (buf[1] << 8));
        }

        public uint U32()
        {
            ReadExact(buf, 4);
            return BigEndian
                ? ((uint)buf[0] << 24) | ((uint)buf[1] << 16) | ((uint)buf[2] << 8) | buf[3]
                : buf[0] | ((uint)buf[1] << 8) | ((uint)buf[2] << 16) | ((uint)buf[3] << 24);
        }

        public uint ReadTag()
        {
            uint g = U16();
            uint e = U16();
            return (g << 16) | e;
        }

        public uint ReadLength(uint tag, out string vr)
        {
            vr = null;
            if (Explicit && (tag >> 16) != 0xFFFE)
            {
                ReadExact(buf, 2);
                vr = new string(new[] { (char)buf[0], (char)buf[1] });
                if (HasLongLength(vr)) { U16(); return U32(); }
                return U16();
            }
            return U32();
        }

        static bool HasLongLength(string vr)
        {
            switch (vr)
            {
                case "OB": case "OD": case "OF": case "OL": case "OV": case "OW":
                case "SQ": case "SV": case "UC": case "UN": case "UR": case "UT": case "UV":
                    return true;
                default:
                    return false;
            }
        }

        public byte[] Bytes(uint n)
        {
            if (n > (1 << 20)) throw new InvalidDataException("Value too long");
            var b = new byte[n];
            ReadExact(b, (int)n);
            return b;
        }

        public void Skip(uint n)
        {
            if (s.CanSeek)
            {
                if (s.Position + n > s.Length) throw new EndOfStreamException();
                s.Seek(n, SeekOrigin.Current);
                return;
            }
            var tmp = new byte[8192];
            long left = n;
            while (left > 0)
            {
                int r = s.Read(tmp, 0, (int)Math.Min(left, tmp.Length));
                if (r <= 0) throw new EndOfStreamException();
                left -= r;
            }
        }

        // Skips a sequence of undefined length (including nested items).
        // A UN of undefined length is always encoded as Implicit VR Little Endian.
        public void SkipSequence(string vr)
        {
            bool saveExplicit = Explicit, saveBigEndian = BigEndian;
            if (vr == "UN") { Explicit = false; BigEndian = false; }
            try
            {
                while (true)
                {
                    uint tag = ReadTag();
                    uint len = U32();
                    if (tag == 0xFFFEE0DD) return;                         // Sequence Delimitation
                    if (tag != 0xFFFEE000) throw new InvalidDataException("Invalid sequence item");
                    if (len == 0xFFFFFFFF) SkipItem(); else Skip(len);
                }
            }
            finally
            {
                Explicit = saveExplicit;
                BigEndian = saveBigEndian;
            }
        }

        void SkipItem()
        {
            while (true)
            {
                uint tag = ReadTag();
                if (tag == 0xFFFEE00D) { U32(); return; }                  // Item Delimitation
                string vr;
                uint len = ReadLength(tag, out vr);
                if (len == 0xFFFFFFFF) SkipSequence(vr); else Skip(len);
            }
        }
    }
}

namespace DicomStudyCount
{
    using System;
    using System.Collections.Generic;
    using System.Drawing;
    using System.Drawing.Drawing2D;
    using System.Drawing.Imaging;
    using System.IO;
    using System.Runtime.InteropServices;

    // Folder icons in the colors of the studies they hold, for the Name column of Explorer (desktop.ini
    // IconResource): an archive box instead of a folder; one study -> a box of its color; 2 to 5 ->
    // vertical stripes; more -> horizontal rainbow stripes (the problems are marked in the comment
    // only). The file name says what the icon is ("c05.ico", "c05-17.ico", "rainbow.ico"), so a lost
    // icon is rebuilt from its name alone.
    public static class FolderIcons
    {
        // Colors chosen to be told apart at 16 px. No yellow: it would look like a plain folder, and
        // it is one of the rainbow stripes.
        public static readonly int[] Palette =
        {
            0x000000, 0x404040, 0x808080, 0xBFBFBF, 0xFFFFFF,   // black, gray 25/50/75 %, white
            0xE6194B, 0x800000, 0xF58231, 0xFFD8B1, 0x9A6324,   // red, maroon, orange, apricot, brown
            0x808000, 0xBFEF45, 0x3CB44B, 0xAAFFC3, 0x469990,   // olive, lime, green, mint, teal
            0x42D4F4, 0x4363D8, 0x000075, 0xDCBEFF, 0x911EB4,   // cyan, blue, navy, lavender, purple
            0xF032E6, 0xFABED4                                  // magenta, pink
        };

        public const int MaxStripes = 5;
        static readonly int[] Sizes = { 16, 20, 24, 32, 40, 48, 64, 256 };

        // Versioned, so a change in the drawing never mixes with icons already cached by Explorer
        public static readonly string Dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"DicomStudyCount\icons");
        static readonly string VersionDir = Path.Combine(Dir, "v2");

        // Always the same color for the same study (64-bit FNV-1a of the StudyInstanceUID)
        public static int ColorIndex(string uid)
        {
            ulong h = 14695981039346656037;
            foreach (char c in uid) { h ^= c; h *= 1099511628211; }
            return (int)((h >> 32) % (ulong)Palette.Length);
        }

        // Icon file name for the color indexes of the studies of a folder (in display order)
        public static string NameFor(IList<int> colors)
        {
            if (colors.Count == 0) return null;
            if (colors.Count > MaxStripes) return "rainbow.ico";
            var parts = new string[colors.Count];
            for (int i = 0; i < colors.Count; i++) parts[i] = colors[i].ToString("D2");
            return "c" + string.Join("-", parts) + ".ico";
        }

        // Also the icons written by another user or on another PC (other profile folder), so a count
        // there replaces them instead of keeping them as someone else's custom icon
        public static bool IsOurs(string iconPath)
        {
            return !string.IsNullOrEmpty(iconPath) &&
                   iconPath.IndexOf(@"\DicomStudyCount\icons\", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        // Full path of the icon, created if it does not exist yet (or was lost)
        public static string Ensure(string name)
        {
            string path = Path.Combine(VersionDir, name);
            if (File.Exists(path)) return path;
            // "-w": the warning sign of earlier versions, no longer drawn
            bool warning = name.EndsWith("-w.ico", StringComparison.OrdinalIgnoreCase);
            string body = name.Substring(0, name.Length - (warning ? 6 : 4));
            List<int> colors = null;     // null = rainbow
            if (body != "rainbow")
            {
                if (!body.StartsWith("c")) throw new ArgumentException("Not an icon name: " + name);
                colors = new List<int>();
                foreach (var part in body.Substring(1).Split('-'))
                {
                    int i = int.Parse(part);
                    if (i < 0 || i >= Palette.Length) throw new ArgumentException("Not an icon name: " + name);
                    colors.Add(i);
                }
            }
            Directory.CreateDirectory(VersionDir);
            // Written under a temporary name and renamed: several threads (or counts) may build it at once
            string tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            WriteIco(tmp, colors);
            try { File.Move(tmp, path); }
            catch (IOException) { File.Delete(tmp); }     // built by someone else meanwhile
            return path;
        }

        // Icon file of a desktop.ini IconResource value ("path,index")
        public static string PathOf(string iconResource)
        {
            if (string.IsNullOrEmpty(iconResource)) return "";
            int comma = iconResource.LastIndexOf(',');
            return comma > 0 ? iconResource.Substring(0, comma) : iconResource;
        }

        // Icon file of a folder written by this script, rebuilt first if it was lost; null if the
        // folder has no icon of ours
        public static string OfFolder(string dir)
        {
            string ini = Path.Combine(dir, "desktop.ini");
            if (!File.Exists(ini)) return null;
            string path = PathOf(Native.ReadIni(ini, ".ShellClassInfo", "IconResource"));
            if (!IsOurs(path)) return null;
            if (!File.Exists(path) &&
                string.Equals(Path.GetDirectoryName(path), VersionDir, StringComparison.OrdinalIgnoreCase))
            {
                try { Ensure(Path.GetFileName(path)); }
                catch (Exception) { return null; }
                Native.NotifyUpdated(dir);
            }
            return File.Exists(path) ? path : null;
        }

        // The image of one of our .ico files at that size (the nearest larger one, scaled down)
        public static Bitmap Load(string path, int size)
        {
            byte[] ico = File.ReadAllBytes(path);
            int count = BitConverter.ToInt16(ico, 4), best = -1, bestSide = int.MaxValue, largest = -1, largestSide = 0;
            for (int i = 0; i < count; i++)
            {
                int side = ico[6 + 16 * i] == 0 ? 256 : ico[6 + 16 * i];
                if (side >= size && side < bestSide) { best = i; bestSide = side; }
                if (side > largestSide) { largest = i; largestSide = side; }
            }
            if (best < 0) best = largest;
            int length = BitConverter.ToInt32(ico, 6 + 16 * best + 8), offset = BitConverter.ToInt32(ico, 6 + 16 * best + 12);
            using (var png = new Bitmap(new MemoryStream(ico, offset, length)))
            {
                var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
                using (var g = Graphics.FromImage(bmp))
                {
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.DrawImage(png, 0, 0, size, size);
                }
                return bmp;
            }
        }

        // Rebuilds the lost icons of the subfolders of a folder (what Explorer shows when browsing it)
        public static void RepairChildren(string dir)
        {
            int n = 0;
            foreach (var sub in Directory.EnumerateDirectories(dir))
            {
                if (n++ >= 2000) break;
                OfFolder(sub);
            }
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern int SHDefExtractIcon(string file, int index, uint flags, out IntPtr large, out IntPtr small, uint size);

        [DllImport("user32.dll")]
        static extern bool DestroyIcon(IntPtr h);

        // The standard folder icon of Windows at that size (imageres.dll, resource 3)
        public static Bitmap SystemFolder(int size)
        {
            IntPtr large, small;
            string imageres = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "imageres.dll");
            if (SHDefExtractIcon(imageres, -3, 0, out large, out small, (uint)size) != 0 || large == IntPtr.Zero)
                throw new InvalidOperationException("Folder icon not found");
            try
            {
                using (var icon = Icon.FromHandle(large))
                using (var bmp = icon.ToBitmap())
                    return new Bitmap(bmp);
            }
            finally { DestroyIcon(large); }
        }

        static void WriteIco(string path, List<int> colors)
        {
            var pngs = new List<byte[]>();
            foreach (int size in Sizes)
                using (var bmp = Draw(size, colors))
                using (var ms = new MemoryStream())
                {
                    bmp.Save(ms, ImageFormat.Png);
                    pngs.Add(ms.ToArray());
                }
            // .ico with PNG entries (supported since Windows Vista)
            using (var w = new BinaryWriter(new FileStream(path, FileMode.CreateNew)))
            {
                w.Write((short)0); w.Write((short)1); w.Write((short)Sizes.Length);
                int offset = 6 + 16 * Sizes.Length;
                for (int i = 0; i < Sizes.Length; i++)
                {
                    byte side = (byte)(Sizes[i] >= 256 ? 0 : Sizes[i]);
                    w.Write(side); w.Write(side); w.Write((byte)0); w.Write((byte)0);
                    w.Write((short)1); w.Write((short)32);
                    w.Write(pngs[i].Length); w.Write(offset);
                    offset += pngs[i].Length;
                }
                foreach (var p in pngs) w.Write(p);
            }
        }

        // An archive box (lid, handle and, from 32 px, a label) drawn on a 16x16 grid scaled to the size:
        // its own shape, so a box of studies is never taken for a plain folder whatever its color
        public static Bitmap Draw(int size, List<int> colors)
        {
            var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            float s = size / 16f;
            using (var g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.ScaleTransform(s, s);
                var bands = Bands(colors, s);
                Paint(g, s, Rounded(1.5f, 5.5f, 13, 9, 0.7f), bands, 0f);
                Paint(g, s, Rounded(0.6f, 2.8f, 14.8f, 3.6f, 0.6f), bands, colors == null ? -0.08f : -0.18f);

                // Handle: a slot darker than the box (lighter on dark colors)
                float at = colors == null ? 8.55f : 8f;
                Color mid = bands.Find(x => x.From <= at && at < x.To).Color;
                using (var b = new SolidBrush(Lum(mid) < 0.25f ? Color.FromArgb(150, 150, 150) : Mix(mid, Color.Black, 0.6f)))
                    g.FillPath(b, Rounded(5.8f, 7.8f, 4.4f, 1.5f, 0.75f));
                if (size >= 32)
                {
                    using (var b = new SolidBrush(Color.FromArgb(235, 255, 255, 255)))
                        g.FillPath(b, Rounded(4.2f, 10.6f, 7.6f, 2.4f, 0.3f));
                    using (var p = new Pen(Color.FromArgb(150, 120, 120, 120), 1f / s))
                        g.DrawPath(p, Rounded(4.2f, 10.6f, 7.6f, 2.4f, 0.3f));
                }
            }
            return bmp;
        }

        // A band of color: the area [From, To) of the grid, vertical (x) or horizontal (y)
        class Band { public float From, To; public Color Color; public bool Horizontal; }

        // The studies as vertical stripes, left to right; more than MaxStripes as horizontal rainbow
        // stripes, so "many" is told from "a few" by the direction and not only by the colors.
        // The edges fall on whole pixels, so the stripes stay sharp at 16 px.
        static List<Band> Bands(List<int> colors, float s)
        {
            var bands = new List<Band>();
            bool rainbow = colors == null;
            int n = rainbow ? Rainbow.Length : colors.Count;
            float from = rainbow ? 2.8f : 1.5f, to = rainbow ? 14.5f : 14.5f;
            for (int i = 0; i < n; i++)
            {
                float a = (float)Math.Round((from + (to - from) * i / n) * s) / s;
                float z = (float)Math.Round((from + (to - from) * (i + 1) / n) * s) / s;
                bands.Add(new Band
                {
                    From = i == 0 ? -1 : a, To = i == n - 1 ? 17 : z, Horizontal = rainbow,
                    Color = rainbow ? Rainbow[i] : FromRgb(Palette[colors[i]])
                });
            }
            return bands;
        }

        static readonly Color[] Rainbow =
        {
            Color.FromArgb(230, 25, 40), Color.FromArgb(250, 135, 20), Color.FromArgb(250, 215, 0),
            Color.FromArgb(40, 170, 60), Color.FromArgb(30, 110, 230), Color.FromArgb(140, 50, 190)
        };

        // Fills a part of the box band by band, shaded (shade < 0: darker), with a 1 px edge in a
        // darker tone of each band (gray for white and for dark colors, visible on any background)
        static void Paint(Graphics g, float s, GraphicsPath path, List<Band> bands, float shade)
        {
            var edge = Inset(path, 0.5f / s);
            foreach (var band in bands)
            {
                g.SetClip(band.Horizontal ? new RectangleF(-1, band.From, 18, band.To - band.From)
                                          : new RectangleF(band.From, -1, band.To - band.From, 18));
                using (var b = new SolidBrush(Tone(band.Color, shade))) g.FillPath(b, path);
                using (var p = new Pen(Edge(band.Color), 1f / s)) g.DrawPath(p, edge);
                g.ResetClip();
            }
        }

        static GraphicsPath Rounded(float x, float y, float w, float h, float r)
        {
            var p = new GraphicsPath();
            float d = 2 * r;
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d, y, d, d, 270, 90);
            p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            p.AddArc(x, y + h - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // The same outline k units inside, so a 1 px edge falls on whole pixels
        static GraphicsPath Inset(GraphicsPath path, float k)
        {
            var b = path.GetBounds();
            var m = new Matrix(b, new[] { new PointF(b.X + k, b.Y + k), new PointF(b.Right - k, b.Y + k), new PointF(b.X + k, b.Bottom - k) });
            var copy = (GraphicsPath)path.Clone();
            copy.Transform(m);
            return copy;
        }

        static Color FromRgb(int rgb) { return Color.FromArgb((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

        static float Lum(Color c) { return (0.299f * c.R + 0.587f * c.G + 0.114f * c.B) / 255f; }

        static Color Mix(Color a, Color b, float t)
        {
            return Color.FromArgb((int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));
        }

        // shade < 0: darker; very dark colors get lighter instead, where darker would not show
        static Color Tone(Color c, float shade)
        {
            if (shade == 0) return c;
            if (Lum(c) < 0.25f) return Mix(c, Color.White, -shade * 0.9f);
            return Mix(c, Color.Black, -shade);
        }

        static Color Edge(Color c)
        {
            float l = Lum(c);
            if (l > 0.8f) return Color.FromArgb(140, 140, 140);
            if (l < 0.25f) return Color.FromArgb(125, 125, 125);
            return Mix(c, Color.Black, 0.4f);
        }
    }
}
