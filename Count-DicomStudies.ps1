<#
.SYNOPSIS
    Counts the distinct DICOM studies (StudyInstanceUID) in a folder and all its subfolders.

.DESCRIPTION
    Reads only the header of each file (it stops at InstanceNumber, tag 0020,0013), so pixel
    data is never loaded. Supports Explicit/Implicit VR Little Endian, Explicit VR Big Endian,
    Deflated and "raw" DICOM files without preamble. Non-DICOM files are ignored.

.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -Detail
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -Csv C:\temp\studies.csv
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -WriteComments    # fills Explorer's Comments column
    .\Count-DicomStudies.ps1 D:\Patients -ClearComments    # clears it
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -ShowTree         # folder tree with the comments
.EXAMPLE
    .\Count-DicomStudies.ps1 -RestoreQuarantine D:\_DicomQuarantine\Patients\20261022_091500
.EXAMPLE
    .\Count-DicomStudies.ps1 -Install      # context menu + background watcher (-Watch) at logon
    .\Count-DicomStudies.ps1 -Uninstall
#>
[CmdletBinding(DefaultParameterSetName = 'Scan')]
param(
    [Parameter(ParameterSetName = 'Scan', Mandatory = $true, Position = 0)]
    [string]$Path,

    # Shows a table with every study (date, modality, patient, description, series, files)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$Detail,

    # Exports the study list to CSV (';' separator so Excel opens it directly in Spanish locales)
    [Parameter(ParameterSetName = 'Scan')]
    [string]$Csv,

    # Writes the study summary into the Comments column of every folder (via desktop.ini)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$WriteComments,

    # Removes the comments written by -WriteComments from the folder and its subfolders
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$ClearComments,

    # Opens a window with the folder tree and the comment of every folder (reads desktop.ini only)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$ShowTree,

    # Waits for a key press when done
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$Pause,

    # Background process: adds the Comments column when browsing folders with DICOM comments
    [Parameter(ParameterSetName = 'Watch', Mandatory = $true)]
    [switch]$Watch,

    # Moves back the files of a quarantine batch folder (the one holding quarantine-log.csv)
    [Parameter(ParameterSetName = 'Restore', Mandatory = $true)]
    [string]$RestoreQuarantine,

    [Parameter(ParameterSetName = 'Install', Mandatory = $true)]
    [switch]$Install,

    [Parameter(ParameterSetName = 'Uninstall', Mandatory = $true)]
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$menuEntries = @(
    @{ Name = 'CountDicomStudies'; Label = 'Count DICOM studies'; Args = '-WriteComments' },
    @{ Name = 'ShowDicomTree'; Label = 'Show DICOM tree'; Args = '-ShowTree' }
)
$menuParents = @(
    'HKCU:\Software\Classes\Directory\shell',
    'HKCU:\Software\Classes\Directory\Background\shell'
)
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'DicomStudyCountWatcher'
# conhost --headless runs PowerShell without opening a console window
$watchCommand = 'conhost.exe --headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Watch' -f $PSCommandPath

function Stop-Watcher {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.CommandLine -like '*Count-DicomStudies.ps1*-Watch*' -and $_.ProcessId -ne $PID } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Install) {
    Set-ItemProperty -LiteralPath $runKey -Name $runName -Value $watchCommand
    Stop-Watcher
    Start-Process conhost.exe -ArgumentList ($watchCommand -replace '^conhost\.exe ', '')
    foreach ($parent in $menuParents) {
        foreach ($entry in $menuEntries) {
            $key = "$parent\$($entry.Name)"
            # conhost --headless runs PowerShell without opening a console window
            $command = 'conhost.exe --headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "%V" {1}' -f $PSCommandPath, $entry.Args
            New-Item -Path "$key\command" -Force | Out-Null
            Set-ItemProperty -LiteralPath $key -Name '(default)' -Value $entry.Label
            Set-ItemProperty -LiteralPath $key -Name 'Icon' -Value 'imageres.dll,-68'
            Set-ItemProperty -LiteralPath "$key\command" -Name '(default)' -Value $command
        }
    }
    Write-Host 'Installed. Right-click a folder:'
    $menuEntries | ForEach-Object { Write-Host "  - $($_.Label)" }
    Write-Host '(on Windows 11 it is under "Show more options" when using the new context menu).'
    Write-Host 'Background watcher running and registered to start at logon.'
    return
}

if ($Uninstall) {
    Remove-ItemProperty -LiteralPath $runKey -Name $runName -ErrorAction SilentlyContinue
    Stop-Watcher
    foreach ($parent in $menuParents) {
        foreach ($entry in $menuEntries) {
            $key = "$parent\$($entry.Name)"
            if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
        }
    }
    Write-Host 'Uninstalled.'
    return
}

function Get-DesktopIniFiles([string]$root) {
    Get-ChildItem -LiteralPath $root -Recurse -Force -Filter 'desktop.ini' -ErrorAction SilentlyContinue |
        Where-Object { -not $_.PSIsContainer }
}

# Adds the Comments column (and fits it to the text) in the Explorer windows open on the
# folder, on its parent folder or on any of its subfolders
function Show-CommentColumn([string]$root) {
    $parent = Split-Path -Path $root -Parent
    # ShellWindows does not enumerate reliably with foreach in PowerShell: iterate by index
    $windows = (New-Object -ComObject Shell.Application).Windows()
    for ($i = 0; $i -lt $windows.Count; $i++) {
        try {
            $window = $windows.Item($i)
            $windowPath = $window.Document.Folder.Self.Path
            if ($windowPath -eq $root -or $windowPath -eq $parent -or $windowPath.StartsWith($root.TrimEnd('\') + '\', 'OrdinalIgnoreCase')) {
                [void][DicomStudyCount.ExplorerColumns]::ShowCommentColumn($window)
            }
        }
        catch { }   # windows that are not file system folders (This PC, Home...)
    }
}

$source = @'
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
        public const string NoDicomText = "No DICOMs.";
        public const string EmptyText = "---";
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

        // Short tag that identifies a study: its main modality and 5 case-sensitive characters from
        // a 64-bit FNV-1a hash of the StudyInstanceUID, always the same for the same study: "CT-aB3x9"
        public static string StudyTag(string uid, string modality)
        {
            ulong h = 14695981039346656037;
            foreach (char c in uid) { h ^= c; h *= 1099511628211; }
            ulong v = h % 916132832UL;      // 62^5
            var chars = new char[5];
            for (int i = 4; i >= 0; i--) { chars[i] = Base62[(int)(v % 62)]; v /= 62; }
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

        public static void Set(string dir, string text)
        {
            string ini = Path.Combine(dir, "desktop.ini");
            lock (LockFor(dir))
            {
                var before = File.GetAttributes(dir);
                bool hadMarker = File.Exists(ini) && Native.ReadIni(ini, Marker, "Generated") != "";
                if (File.Exists(ini))
                {
                    // Drop the Unicode copy of the previous text: the API only rewrites it for non-ASCII text
                    File.SetAttributes(ini, FileAttributes.Normal);
                    Native.DeleteIniKey(ini, ".ShellClassInfo.W", "InfoTip");
                    Native.DeleteIniKey(ini, ".ShellClassInfo.A", "InfoTip");
                }

                // Using the official API (rather than writing the .ini by hand) is what makes
                // Explorer invalidate its desktop.ini cache and refresh the column right away
                Native.SetFolderInfoTip(dir, text);

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

        public static void SetFolderInfoTip(string dir, string text)
        {
            var s = new SHFOLDERCUSTOMSETTINGS
            {
                dwSize = (uint)Marshal.SizeOf(typeof(SHFOLDERCUSTOMSETTINGS)),
                dwMask = 0x4,           // FCSM_INFOTIP
                pszInfoTip = text
            };
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
                        TrySetComment(node.Path, node.TotalFiles == 0 ? FolderComments.EmptyText : FolderComments.NoDicomText, p);
                    else
                    {
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
                        TrySetComment(node.Path, node.BaseComment, p);
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
            if (indexDir != null) CheckOtherLocations(root, indexDir, indexEntries, nodeByPath, result);

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
                                        Dictionary<string, FolderNode> nodeByPath, ScanResult result)
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

        static void TrySetComment(string dir, string text, ScanProgress p)
        {
            try { FolderComments.Set(dir, text); }
            catch (Exception) { Interlocked.Increment(ref p.CommentErrors); }
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
    internal static class GlobalIndex
    {
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
        const uint TagTransferSyntax = 0x00020010;
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

        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        // null = not DICOM. Throws if it is DICOM but cannot be read.
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
                    return string.IsNullOrEmpty(t.Uid) ? null : t;
                }
                catch (Exception) { return null; }
            }
        }

        static DicomTags ReadAfterPreamble(FileStream fs)
        {
            // File meta information (group 0002): always Explicit VR Little Endian
            var r = new DicomReader(fs) { Explicit = true };
            string ts = null;
            while (fs.Position + 8 <= fs.Length)
            {
                long pos = fs.Position;
                uint tag = r.ReadTag();
                if ((tag >> 16) != 0x0002) { fs.Position = pos; break; }
                string vr;
                uint len = r.ReadLength(tag, out vr);
                if (tag == TagTransferSyntax) ts = Str(r.Bytes(len));
                else r.Skip(len);
            }

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
                if (len == 0xFFFFFFFF) { r.SkipSequence(vr); continue; }
                switch (tag)
                {
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
'@

if (-not ('DicomStudyCount.Scanner' -as [type])) {
    # Compiling takes ~2 s, so the DLL is kept in %LOCALAPPDATA%, named after a hash of the source
    $binDir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\bin'
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($source))[0..7] | ForEach-Object { $_.ToString('x2') })
    $dll = Join-Path $binDir "DicomStudyCount-$hash.dll"
    try {
        if (-not (Test-Path -LiteralPath $dll)) {
            New-Item -ItemType Directory -Path $binDir -Force | Out-Null
            # Compiled under a temporary name and renamed, in case two counts start at the same time
            $tmp = Join-Path $binDir ([Guid]::NewGuid().ToString('N') + '.dll')
            Add-Type -TypeDefinition $source -Language CSharp -OutputAssembly $tmp -OutputType Library
            try { Move-Item -LiteralPath $tmp -Destination $dll -ErrorAction Stop }
            catch { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
            # DLLs of older versions of the script (the ones still loaded are left for the next time)
            Get-ChildItem -LiteralPath $binDir -Filter '*.dll' | Where-Object { $_.FullName -ne $dll } |
                Remove-Item -Force -ErrorAction SilentlyContinue
        }
        Add-Type -LiteralPath $dll
    }
    catch {
        if (-not ('DicomStudyCount.Scanner' -as [type])) { Add-Type -TypeDefinition $source -Language CSharp }
    }
}

# Problems reports (split studies, duplicated files) live outside the scanned folders,
# one JSON file per scanned root
$reportDir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\reports'
# Index of counted locations, to find copies across them (one file per counted folder)
$indexDir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\index'

function Get-ReportPath([string]$root) {
    $sha = [Security.Cryptography.SHA1]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant())) | ForEach-Object { $_.ToString('x2') })
    Join-Path $reportDir "$hash.json"
}

function Save-ProblemsReport([string]$root, $result) {
    New-Item -ItemType Directory -Force $reportDir | Out-Null
    $report = [ordered]@{
        Root            = $root
        ScannedAt       = (Get-Date).ToString('s')
        DuplicatedFiles = $result.DuplicatedFiles
        Splits          = @($result.Splits | ForEach-Object { [ordered]@{ Tag = $_.Tag; StudyInstanceUID = $_.StudyInstanceUID; Folders = @($_.Folders) } })
        Duplicates      = @($result.Duplicates | ForEach-Object {
            [ordered]@{
                Files     = $_.Files
                Identical = $_.Identical
                Different = $_.Different
                Keep      = $_.Keep
                Folders   = @($_.Folders)
                Groups    = @($_.Groups | ForEach-Object { [ordered]@{ Identical = $_.Identical; Files = @($_.Files) } })
            }
        })
        Incomplete      = @($result.Incomplete | ForEach-Object {
            [ordered]@{ Tag = $_.Tag; Series = $_.Series; Folder = $_.Folder; Have = $_.Have; Expected = $_.Expected; Missing = $_.Missing }
        })
        External        = @($result.External | ForEach-Object {
            [ordered]@{ Files = $_.Files; Identical = $_.Identical; Different = $_.Different; Folder = $_.Folder; OtherFolder = $_.OtherFolder }
        })
    }
    $report | ConvertTo-Json -Depth 10 -Compress | Set-Content -LiteralPath (Get-ReportPath $root) -Encoding UTF8
}

# Quarantine: identical duplicated files are moved (never deleted) to
# <parent of the counted folder>\_DicomQuarantine\<folder name>\<date_time>\<same relative path>,
# with a log to undo it. Folders named _DicomQuarantine are never counted.
function Get-QuarantineBase([string]$root) {
    $folder = $root.TrimEnd('\')
    $parent = Split-Path -Path $folder -Parent
    if (-not $parent) { return Join-Path "$folder\_DicomQuarantine" $folder.Substring(0, 1) }   # drive root
    Join-Path (Join-Path $parent '_DicomQuarantine') (Split-Path -Path $folder -Leaf)
}

# Moves every identical copy outside the set's Keep folder (one copy of each image always stays).
# Each file is compared again right before moving it, since it may have changed after the count.
function Invoke-Quarantine([string]$root, $report) {
    $folder = $root.TrimEnd('\')
    $batch = Join-Path (Get-QuarantineBase $root) (Get-Date -Format 'yyyyMMdd_HHmmss')
    $log = New-Object Collections.Generic.List[object]
    $skipped = 0; $failed = 0
    foreach ($set in $report.Duplicates) {
        foreach ($group in $set.Groups) {
            if (-not $group.Identical) { continue }
            $files = @($group.Files | Where-Object { [IO.File]::Exists($_) } | Sort-Object)
            if ($files.Count -lt 2) { continue }
            $keepFile = @($files | Where-Object { [IO.Path]::GetDirectoryName($_) -eq $set.Keep })[0]
            if (-not $keepFile) { $keepFile = $files[0] }
            foreach ($file in $files) {
                if ($file -eq $keepFile) { continue }
                if (-not $file.StartsWith($folder + '\', 'OrdinalIgnoreCase') -or
                    -not [DicomStudyCount.FileCompare]::Identical($keepFile, $file)) { $skipped++; continue }
                $dest = Join-Path $batch $file.Substring($folder.Length + 1)
                try {
                    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($dest)) | Out-Null
                    Move-Item -LiteralPath $file -Destination $dest
                    $log.Add([pscustomobject]@{ Original = $file; Quarantined = $dest; KeptCopy = $keepFile })
                }
                catch { $failed++ }
            }
        }
    }
    if ($log.Count) { $log | Export-Csv -LiteralPath (Join-Path $batch 'quarantine-log.csv') -NoTypeInformation -Encoding UTF8 }
    [pscustomobject]@{ Moved = $log.Count; Skipped = $skipped; Failed = $failed; Batch = $batch }
}

# Moves the files of a quarantine batch back to where they were (unless something is there again)
function Restore-Quarantine([string]$batch) {
    $logFile = Join-Path $batch 'quarantine-log.csv'
    $restored = 0; $skipped = 0
    foreach ($entry in Import-Csv -LiteralPath $logFile -Encoding UTF8) {
        if ([IO.File]::Exists($entry.Original) -or -not [IO.File]::Exists($entry.Quarantined)) { $skipped++; continue }
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($entry.Original)) | Out-Null
        Move-Item -LiteralPath $entry.Quarantined -Destination $entry.Original
        $restored++
    }
    Rename-Item -LiteralPath $logFile -NewName 'restored-log.csv'
    [pscustomobject]@{ Restored = $restored; Skipped = $skipped }
}

# Latest quarantine batch of a folder that has not been undone yet
function Get-LastQuarantine([string]$root) {
    $base = Get-QuarantineBase $root
    if (-not (Test-Path -LiteralPath $base)) { return $null }
    Get-ChildItem -LiteralPath $base -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'quarantine-log.csv') } |
        Sort-Object Name | Select-Object -Last 1
}

# Latest report covering the folder (made on it or on an ancestor), keeping only what concerns it
function Get-ProblemsReport([string]$root) {
    if (-not (Test-Path -LiteralPath $reportDir)) { return $null }
    $folder = $root.TrimEnd('\')
    $best = $null
    foreach ($file in Get-ChildItem -LiteralPath $reportDir -Filter '*.json') {
        try { $r = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json } catch { continue }
        $reportRoot = ([string]$r.Root).TrimEnd('\')
        if ($folder -ne $reportRoot -and -not $folder.StartsWith($reportRoot + '\', 'OrdinalIgnoreCase')) { continue }
        if (-not $best -or $r.ScannedAt -gt $best.ScannedAt) { $best = $r }
    }
    if (-not $best) { return $null }
    $concerns = { param($paths) @($paths | Where-Object { $_ -eq $folder -or $_.StartsWith($folder + '\', 'OrdinalIgnoreCase') }).Count -gt 0 }
    [pscustomobject]@{
        Root       = $best.Root
        ScannedAt  = $best.ScannedAt
        Splits     = @($best.Splits | Where-Object { & $concerns $_.Folders })
        Duplicates = @($best.Duplicates | Where-Object { & $concerns $_.Folders })
        Incomplete = @($best.Incomplete | Where-Object { $_ -and (& $concerns @($_.Folder)) })
        External   = @($best.External | Where-Object { $_ -and (& $concerns @($_.Folder)) })
    }
}

# Does any subfolder have a comment written by this script? (checks 500 at most)
function Test-HasDicomComments([string]$dir) {
    $n = 0
    try {
        foreach ($sub in [IO.Directory]::EnumerateDirectories($dir)) {
            if ($n++ -ge 500) { break }
            $ini = [IO.Path]::Combine($sub, 'desktop.ini')
            if ([IO.File]::Exists($ini) -and [DicomStudyCount.Native]::ReadIni($ini, 'DicomStudyCount', 'Generated')) { return $true }
        }
    }
    catch { }
    return $false
}

# Window with the folder tree and the comment of every folder next to it. Reads desktop.ini only:
# it neither reads DICOM files nor writes anything.
function Show-DicomTree([string]$root) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    [void][DicomStudyCount.Native]::SetProcessDPIAware()
    [Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object Windows.Forms.Form
    $form.Text = "DICOM tree - $root"
    $form.Size = New-Object Drawing.Size(1250, 800)
    $form.StartPosition = 'CenterScreen'
    $form.Font = New-Object Drawing.Font('Segoe UI', 9)
    $form.KeyPreview = $true

    $tree = New-Object Windows.Forms.TreeView
    $tree.Dock = 'Fill'
    $tree.DrawMode = 'OwnerDrawText'
    $tree.HideSelection = $false
    $tree.ShowNodeToolTips = $true
    $tree.ItemHeight = [int]($form.Font.Height * 1.6)

    $toolbar = New-Object Windows.Forms.FlowLayoutPanel
    $toolbar.Dock = 'Top'
    $toolbar.AutoSize = $true
    $toolbar.Padding = New-Object Windows.Forms.Padding(4)
    $script:scriptPath = $PSCommandPath
    $countRoot = { Start-Process conhost.exe -ArgumentList ('--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "{1}" -WriteComments' -f $script:scriptPath, $root) }
    $expandToStudies = {
        if (-not $tree.Nodes.Count) { return }
        $tree.BeginUpdate()
        Expand-ToStudies $tree.Nodes[0]
        $tree.EndUpdate()
        $tree.Nodes[0].EnsureVisible()
    }
    foreach ($b in @(@('Count DICOM studies', $countRoot),
                     @('Expand to studies', $expandToStudies),
                     @('Expand all', { $tree.ExpandAll(); if ($tree.Nodes.Count) { $tree.Nodes[0].EnsureVisible() } }),
                     @('Collapse all', { $tree.CollapseAll(); if ($tree.Nodes.Count) { $tree.Nodes[0].Expand() } }),
                     @('Refresh (F5)', { Update-Tree }),
                     @('Problems', { Show-Problems }))) {
        $button = New-Object Windows.Forms.Button
        $button.Text = $b[0]
        $button.AutoSize = $true
        $button.add_Click($b[1])
        $toolbar.Controls.Add($button)
        if ($b[0] -eq 'Problems') { $problemsButton = $button }
    }

    # Updates the Problems button with the number of problems in the latest report
    function Update-ProblemsButton {
        $report = Get-ProblemsReport $root
        $count = if ($report) { $report.Splits.Count + $report.Duplicates.Count + $report.Incomplete.Count + $report.External.Count } else { 0 }
        $problemsButton.Text = if ($count) { "Problems ($count)" } else { 'Problems' }
        $problemsButton.ForeColor = if ($count) { [Drawing.Color]::FromArgb(180, 0, 0) } else { [Drawing.SystemColors]::ControlText }
    }

    # The quarantine of identical duplicates (Invoke-Quarantine / Restore-Quarantine) is implemented but
    # its interface is disabled for now: the Problems window only reports. Set to $true to show the
    # quarantine buttons and the right-click "Keep the copies in this folder".
    $script:quarantineEnabled = $false

    # Folder nodes of a duplicate set: the folder suggested to keep the copies is marked in bold green
    function Update-SetNode($setNode) {
        $set = $setNode.Tag
        $mark = if ($script:quarantineEnabled) { '(keep)' } else { '(suggested keep)' }
        foreach ($child in $setNode.Nodes) {
            if ($child.Tag -eq $set.Keep) {
                $child.Text = "$mark  $($child.Tag)"
                $child.NodeFont = New-Object Drawing.Font($tree.Font, [Drawing.FontStyle]::Bold)
                $child.ForeColor = [Drawing.Color]::FromArgb(0, 120, 40)
            } else {
                $child.Text = $child.Tag
                $child.NodeFont = $null
                $child.ForeColor = [Drawing.Color]::Empty
            }
        }
    }

    $quarantineClick = {
        $report = $script:problemsReport
        $toMove = 0
        foreach ($set in $report.Duplicates) {
            foreach ($g in $set.Groups) { if ($g.Identical) { $toMove += @($g.Files).Count - 1 } }
        }
        if (-not $toMove) {
            [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, 'There are no identical copies to quarantine.', 'Quarantine')
            return
        }
        $message = "Move $toMove identical copies to:`n$(Get-QuarantineBase $root)\<date_time>`n`n" +
                   "One copy of each image stays, in the folders marked (keep).`n" +
                   "Files with the same SOPInstanceUID but different contents are not touched.`n" +
                   "Nothing is deleted: 'Undo last quarantine' moves them back."
        $answer = [Windows.Forms.MessageBox]::Show($script:problemsDialog, $message, 'Quarantine identical duplicates', 'YesNo', 'Warning')
        if ($answer -ne 'Yes') { return }
        $script:problemsDialog.Cursor = 'WaitCursor'
        $r = Invoke-Quarantine $root $report
        $script:problemsDialog.Cursor = 'Default'
        [void][Windows.Forms.MessageBox]::Show($script:problemsDialog,
            ("Moved: {0}`nSkipped (changed since the count, or outside this folder): {1}`nFailed: {2}`n`nQuarantine folder:`n{3}`n`nThe folder is counted again now to update the comments." -f $r.Moved, $r.Skipped, $r.Failed, $r.Batch),
            'Quarantine')
        $script:problemsDialog.Close()
        & $countRoot
    }

    $undoClick = {
        $last = Get-LastQuarantine $root
        if (-not $last) {
            [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, 'There is no quarantine to undo for this folder.', 'Undo')
            return
        }
        $answer = [Windows.Forms.MessageBox]::Show($script:problemsDialog,
            "Move back the files quarantined in:`n$($last.FullName)?", 'Undo last quarantine', 'YesNo', 'Question')
        if ($answer -ne 'Yes') { return }
        $r = Restore-Quarantine $last.FullName
        [void][Windows.Forms.MessageBox]::Show($script:problemsDialog,
            ("Restored: {0}`nSkipped (already back or missing): {1}`n`nThe folder is counted again now to update the comments." -f $r.Restored, $r.Skipped),
            'Undo last quarantine')
        $script:problemsDialog.Close()
        & $countRoot
    }

    # Window with the latest problems report: studies split across folders and duplicated files,
    # with the quarantine of identical copies. Clicking a folder selects it in the tree,
    # double-clicking opens it in Explorer, right-clicking a folder of a duplicate set chooses it as
    # the one that keeps the copies. (Modeless: what its events need is kept in script variables.)
    function Show-Problems {
        $report = Get-ProblemsReport $root
        if (-not $report) {
            [void][Windows.Forms.MessageBox]::Show($form, 'There is no report for this folder yet: click "Count DICOM studies" first.', 'Problems')
            return
        }
        if ($script:problemsDialog -and -not $script:problemsDialog.IsDisposed) { $script:problemsDialog.Close() }
        $script:problemsReport = $report

        $dialog = New-Object Windows.Forms.Form
        $dialog.Text = "Problems - $root  (counted $($report.ScannedAt -replace 'T', ' '))"
        $dialog.Size = New-Object Drawing.Size(1150, 700)
        $dialog.StartPosition = 'CenterParent'
        $dialog.Font = $form.Font
        $script:problemsDialog = $dialog

        $bar = New-Object Windows.Forms.FlowLayoutPanel
        $bar.Dock = 'Top'
        $bar.AutoSize = $true
        $bar.Padding = New-Object Windows.Forms.Padding(4)
        foreach ($b in @(@('Quarantine identical duplicates...', $quarantineClick),
                         @('Undo last quarantine', $undoClick),
                         @('Open quarantine folder', {
                             $base = Get-QuarantineBase $root
                             if (Test-Path -LiteralPath $base) { Start-Process explorer.exe ('"{0}"' -f $base) }
                             else { [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, "Nothing quarantined yet.`n($base)", 'Quarantine') }
                         }))) {
            $button = New-Object Windows.Forms.Button
            $button.Text = $b[0]
            $button.AutoSize = $true
            $button.add_Click($b[1])
            $bar.Controls.Add($button)
        }
        $bar.Visible = $script:quarantineEnabled

        $list = New-Object Windows.Forms.TreeView
        $list.Dock = 'Fill'
        $list.HideSelection = $false
        $list.ItemHeight = $tree.ItemHeight

        $splits = $list.Nodes.Add("Studies split across folders ($($report.Splits.Count))")
        foreach ($s in $report.Splits) {
            $sn = $splits.Nodes.Add("[$($s.Tag)] $($s.StudyInstanceUID)  -  in $(@($s.Folders).Count) folders")
            foreach ($folder in $s.Folders) { $fn = $sn.Nodes.Add($folder); $fn.Tag = $folder }
        }
        $dupFiles = [int]($report.Duplicates | Measure-Object -Property Files -Sum).Sum
        $dupsLabel = "Duplicated files, same StudyInstanceUID and SOPInstanceUID ($dupFiles files in $($report.Duplicates.Count) folder sets)"
        if ($script:quarantineEnabled) { $dupsLabel += '  -  right-click a folder to keep the copies there' }
        $dups = $list.Nodes.Add($dupsLabel)
        foreach ($d in $report.Duplicates) {
            $where = if (@($d.Folders).Count -eq 1) { 'inside one folder' } else { "shared by $(@($d.Folders).Count) folders" }
            $dn = $dups.Nodes.Add("$($d.Files) files $where  -  $($d.Identical) byte-identical, $($d.Different) with different contents")
            if ($d.Different) { $dn.ForeColor = [Drawing.Color]::FromArgb(180, 0, 0) }
            $dn.Tag = $d
            foreach ($folder in $d.Folders) { $fn = $dn.Nodes.Add($folder); $fn.Tag = $folder }
            Update-SetNode $dn
        }
        $incomplete = $list.Nodes.Add("Incomplete series ($($report.Incomplete.Count))  -  InstanceNumbers missing in a folder, compared with the whole tree")
        foreach ($s in $report.Incomplete) {
            $sn = $incomplete.Nodes.Add("[$($s.Tag)] $($s.Series)  -  $($s.Have) of $($s.Expected), missing $($s.Missing)")
            $fn = $sn.Nodes.Add($s.Folder); $fn.Tag = $s.Folder
        }
        $externalFiles = [int]($report.External | Measure-Object -Property Files -Sum).Sum
        $external = $list.Nodes.Add("Also in other counted locations ($externalFiles files in $($report.External.Count) folder pairs)")
        foreach ($x in $report.External) {
            $xn = $external.Nodes.Add("$($x.Files) files  -  $($x.Identical) byte-identical, $($x.Different) with different contents")
            if ($x.Different) { $xn.ForeColor = [Drawing.Color]::FromArgb(180, 0, 0) }
            $fn = $xn.Nodes.Add($x.Folder); $fn.Tag = $x.Folder
            $on = $xn.Nodes.Add("elsewhere:  $($x.OtherFolder)"); $on.Tag = $x.OtherFolder
        }
        $splits.ExpandAll()
        $dups.ExpandAll()
        $incomplete.ExpandAll()
        $external.ExpandAll()
        if ($list.Nodes.Count) { $list.Nodes[0].EnsureVisible() }

        $menu = New-Object Windows.Forms.ContextMenuStrip
        $keepItem = $menu.Items.Add('Keep the copies in this folder')
        $keepItem.add_Click({
            $node = $script:menuNode
            $node.Parent.Tag.Keep = $node.Tag
            Update-SetNode $node.Parent
        })
        $list.add_NodeMouseClick({
            param($sender, $e)
            if ($e.Button -ne 'Right' -or -not $script:quarantineEnabled) { return }
            $sender.SelectedNode = $e.Node
            # Only folders of a duplicate set (their parent holds the set, which has Groups)
            if ($e.Node.Tag -is [string] -and $e.Node.Parent -and $e.Node.Parent.Tag -and $e.Node.Parent.Tag.PSObject.Properties['Groups']) {
                $script:menuNode = $e.Node
                $script:problemsMenu.Show($sender, $e.Location)
            }
        })
        $script:problemsMenu = $menu
        $list.add_AfterSelect({
            param($sender, $e)
            $node = if ($e.Node.Tag -is [string]) { $script:nodeByPath[$e.Node.Tag] } else { $null }
            if ($node) { $tree.SelectedNode = $node; $node.EnsureVisible() }
        })
        $list.add_NodeMouseDoubleClick({
            param($sender, $e)
            if ($e.Node.Tag -is [string]) { Start-Process explorer.exe ('"{0}"' -f $e.Node.Tag) }
        })
        $dialog.Controls.Add($list)
        $dialog.Controls.Add($bar)
        $dialog.Show($form)
    }

    $statusStrip = New-Object Windows.Forms.StatusStrip
    $statusLabel = New-Object Windows.Forms.ToolStripStatusLabel
    [void]$statusStrip.Items.Add($statusLabel)

    $form.Controls.Add($tree)
    $form.Controls.Add($toolbar)
    $form.Controls.Add($statusStrip)

    # Natural order: "2" before "10"
    $naturalKey = { [regex]::Replace([IO.Path]::GetFileName($_), '\d+', { $args[0].Value.PadLeft(12, '0') }) }

    function Get-FolderComment([string]$dir) {
        $ini = [IO.Path]::Combine($dir, 'desktop.ini')
        if (-not [IO.File]::Exists($ini)) { return '' }
        $text = [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo', 'InfoTip')
        # Non-ASCII text (such as the warning sign) is also stored UTF-7 encoded in .ShellClassInfo.W,
        # valid only while the .ShellClassInfo.A copy matches the main text (same rule as Windows)
        $unicode = [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo.W', 'InfoTip')
        if ($unicode -and [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo.A', 'InfoTip') -eq $text) {
            return [Text.Encoding]::UTF7.GetString([Text.Encoding]::ASCII.GetBytes($unicode))
        }
        $text
    }

    # Files directly in the folder (the desktop.ini written by this script does not count)
    function Get-DirectFileCount([string]$dir) {
        try { $n = [IO.Directory]::GetFiles($dir).Length } catch { return 0 }
        if ([IO.File]::Exists([IO.Path]::Combine($dir, 'desktop.ini'))) { $n-- }
        $n
    }

    # Adds the folder and its whole subtree and returns its node; $index inserts it at a
    # position instead of at the end. TotalFiles counts the files of the whole subtree.
    function Add-FolderNode($nodes, [string]$dir, [string]$name, [int]$level, [int]$index = -1) {
        $comment = Get-FolderComment $dir
        $direct = Get-DirectFileCount $dir
        $node = New-Object Windows.Forms.TreeNode($name)
        if ($index -ge 0) { $nodes.Insert($index, $node) } else { [void]$nodes.Add($node) }
        $node.Tag = [pscustomobject]@{ Path = $dir; Comment = $comment; StudyUid = (Get-StudyUid $comment); DirectFiles = $direct; TotalFiles = $direct }
        $node.ToolTipText = $comment
        $script:nodeByPath[$dir] = $node
        $width = ($level + 1) * $tree.Indent + [Windows.Forms.TextRenderer]::MeasureText($name, $tree.Font).Width
        if ($width -gt $script:nameWidth) { $script:nameWidth = $width }
        $subs = @()
        try { $subs = [IO.Directory]::GetDirectories($dir) | Sort-Object $naturalKey } catch { }
        foreach ($sub in $subs) {
            $child = Add-FolderNode $node.Nodes $sub ([IO.Path]::GetFileName($sub)) ($level + 1)
            $node.Tag.TotalFiles += $child.Tag.TotalFiles
        }
        $node
    }

    function Remove-FromIndex($node) {
        $script:nodeByPath.Remove($node.Tag.Path)
        foreach ($child in $node.Nodes) { Remove-FromIndex $child }
    }

    function Repaint-Row($node) {
        if ($node.IsVisible) {
            $b = $node.Bounds
            $tree.Invalidate((New-Object Drawing.Rectangle(0, $b.Y, $tree.ClientSize.Width, $b.Height)))
        }
    }

    # Adds a change in the number of files to the folder and all its ancestors
    function Add-FileDelta($node, [int]$delta) {
        if ($delta -eq 0) { return }
        for ($n = $node; $n; $n = $n.Parent) {
            $n.Tag.TotalFiles += $delta
            Repaint-Row $n
        }
    }

    # Makes the children of a node match the subfolders currently on disk (same natural order)
    function Sync-Children($node) {
        $subs = @()
        try { $subs = @([IO.Directory]::GetDirectories($node.Tag.Path) | Sort-Object $naturalKey) } catch { }
        $onDisk = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($s in $subs) { [void]$onDisk.Add($s) }
        foreach ($child in @($node.Nodes)) {
            if (-not $onDisk.Contains($child.Tag.Path)) {
                Remove-FromIndex $child
                $child.Remove()
                Add-FileDelta $node (-$child.Tag.TotalFiles)
            }
        }
        for ($i = 0; $i -lt $subs.Count; $i++) {
            if ($i -lt $node.Nodes.Count -and $node.Nodes[$i].Tag.Path -eq $subs[$i]) { continue }
            $added = Add-FolderNode $node.Nodes $subs[$i] ([IO.Path]::GetFileName($subs[$i])) ($node.Level + 1) $i
            Add-FileDelta $node $added.Tag.TotalFiles
        }
    }

    # Single-study folders have comments starting with their study tag, "[CT-xxxxx] dd/MMM/yyyy ...";
    # the tag identifies the study in the tree (colors, "Expand to studies")
    function Get-StudyUid([string]$comment) {
        if ($comment -cmatch '^\[([A-Z0-9]+-[0-9A-Za-z]{5})\] ') { return $Matches[1] }
        $null
    }

    # Soft pastel color for the i-th study: the hue advances by the golden angle, so consecutive
    # studies always get clearly different hues; lightness is high enough for black text.
    function Get-PastelColor([int]$index) {
        $h = (($index * 137.508) % 360) / 360.0
        $s = 0.70; $l = 0.86
        $q = $l + $s - $l * $s
        $p = 2 * $l - $q
        $rgb = foreach ($t in ($h + 1 / 3), $h, ($h - 1 / 3)) {
            if ($t -lt 0) { $t += 1 }
            if ($t -gt 1) { $t -= 1 }
            $v = if ($t -lt 1 / 6) { $p + ($q - $p) * 6 * $t }
                 elseif ($t -lt 1 / 2) { $q }
                 elseif ($t -lt 2 / 3) { $p + ($q - $p) * (2 / 3 - $t) * 6 }
                 else { $p }
            [int][Math]::Round($v * 255)
        }
        [Drawing.Color]::FromArgb($rgb[0], $rgb[1], $rgb[2])
    }

    # Assigns a color to every study in tree order (pre-order, natural sort), so the same folder
    # always gets the same colors; repaints only if the assignment changed
    function Update-StudyColors {
        $order = New-Object Collections.Generic.List[string]
        $seen = New-Object 'System.Collections.Generic.HashSet[string]'
        $stack = New-Object System.Collections.Stack
        for ($i = $tree.Nodes.Count - 1; $i -ge 0; $i--) { $stack.Push($tree.Nodes[$i]) }
        while ($stack.Count) {
            $n = $stack.Pop()
            $uid = $n.Tag.StudyUid
            if ($uid -and $seen.Add($uid)) { $order.Add($uid) }
            for ($i = $n.Nodes.Count - 1; $i -ge 0; $i--) { $stack.Push($n.Nodes[$i]) }
        }
        $current = if ($script:studyOrder) { $script:studyOrder -join '|' } else { '' }
        if (($order -join '|') -eq $current) { return }
        $script:studyOrder = $order.ToArray()
        if ($script:studyBrushes) { foreach ($b in $script:studyBrushes.Values) { $b.Dispose() } }
        $script:studyBrushes = New-Object Collections.Hashtable     # case-sensitive: tags differ by case
        for ($i = 0; $i -lt $order.Count; $i++) {
            $script:studyBrushes[$order[$i]] = New-Object Drawing.SolidBrush (Get-PastelColor $i)
        }
        $tree.Invalidate()
    }

    # Expands folders holding several studies and stops at the first single-study level;
    # folders without DICOM stay collapsed
    function Expand-ToStudies($node) {
        if ($node.Tag.StudyUid -or $node.Tag.Comment -notmatch '^\d+ DICOM studies') {
            $node.Collapse()
            return
        }
        $node.Expand()
        foreach ($child in $node.Nodes) { Expand-ToStudies $child }
    }

    function Update-Status {
        $commented = 0
        foreach ($n in $script:nodeByPath.Values) { if ($n.Tag.Comment) { $commented++ } }
        $rootComment = if ($tree.Nodes.Count) { $tree.Nodes[0].Tag.Comment } else { '' }
        $files = if ($tree.Nodes.Count) { '{0:N0} files  -  ' -f $tree.Nodes[0].Tag.TotalFiles } else { '' }
        if ($script:studyOrder -and $script:studyOrder.Count) { $files += '{0} studies  -  ' -f $script:studyOrder.Count }
        $statusLabel.Text = $files + $(if ($rootComment -like 'Counting*') {
            "Counting... the tree updates as each folder is finished"
        } elseif ($commented) {
            "$commented folders with comments  -  double-click: open in Explorer  -  F5: refresh"
        } else {
            'No comments found: click "Count DICOM studies" to count this folder'
        })
    }

    # Rebuilds the tree keeping the expanded folders and the selection
    function Update-Tree {
        $expanded = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $stack = New-Object System.Collections.Stack
        foreach ($n in $tree.Nodes) { $stack.Push($n) }
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($n.IsExpanded) { [void]$expanded.Add($n.Tag.Path) }
            foreach ($c in $n.Nodes) { $stack.Push($c) }
        }
        $selected = if ($tree.SelectedNode) { $tree.SelectedNode.Tag.Path } else { $null }

        $form.Cursor = 'WaitCursor'
        $tree.BeginUpdate()
        $tree.Nodes.Clear()
        $script:nodeByPath = New-Object Collections.Hashtable ([StringComparer]::OrdinalIgnoreCase)
        $script:nameWidth = 0
        [void](Add-FolderNode $tree.Nodes $root $root 0)
        # Common columns right after the widest folder name: number of files, then the comment
        $script:filesX = [Math]::Min($script:nameWidth + 30, 560)
        $script:filesW = [Windows.Forms.TextRenderer]::MeasureText('9,999,999 files', $tree.Font).Width
        $script:commentX = $script:filesX + $script:filesW + 24

        $stack.Clear()
        foreach ($n in $tree.Nodes) { $stack.Push($n) }
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($expanded.Contains($n.Tag.Path)) { $n.Expand() }
            if ($selected -and $n.Tag.Path -eq $selected) { $tree.SelectedNode = $n }
            foreach ($c in $n.Nodes) { $stack.Push($c) }
        }
        if ($expanded.Count -eq 0) { $tree.Nodes[0].Expand() }
        $tree.EndUpdate()
        $form.Cursor = 'Default'
        Update-StudyColors
        Update-Status
        Update-ProblemsButton
    }

    $tree.add_DrawNode({
        param($sender, $e)
        if ($e.Bounds.IsEmpty) { return }
        $selected = ($e.State -band [Windows.Forms.TreeNodeStates]::Selected) -ne 0
        $flags = [Windows.Forms.TextFormatFlags]'VerticalCenter, Left, NoPrefix, SingleLine'
        $nameColor = $tree.ForeColor
        # Single-study rows: pastel background with the color of their study, from the name to the edge
        $uid = $e.Node.Tag.StudyUid
        $studyBrush = if ($uid -and $script:studyBrushes) { $script:studyBrushes[$uid] } else { $null }
        if ($studyBrush) {
            $e.Graphics.FillRectangle($studyBrush, $e.Bounds.X, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $e.Bounds.X), $e.Bounds.Height)
        }
        if ($selected) {
            $e.Graphics.FillRectangle([Drawing.SystemBrushes]::Highlight, $e.Bounds)
            $nameColor = [Drawing.SystemColors]::HighlightText
        }
        [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $e.Node.Text, $tree.Font, $e.Bounds, $nameColor, $flags)

        # Number of files, right-aligned in its own column
        $total = $e.Node.Tag.TotalFiles
        $filesText = if ($total -eq 1) { '1 file' } else { '{0:N0} files' -f $total }
        $filesRect = New-Object Drawing.Rectangle([Math]::Max($script:filesX, $e.Bounds.Right + 8), $e.Bounds.Y, $script:filesW, $e.Bounds.Height)
        $filesFlags = [Windows.Forms.TextFormatFlags]'VerticalCenter, Right, NoPrefix, SingleLine'
        $filesColor = if ($studyBrush) { [Drawing.Color]::FromArgb(60, 60, 60) } else { [Drawing.Color]::DimGray }
        [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $filesText, $tree.Font, $filesRect, $filesColor, $filesFlags)

        $comment = $e.Node.Tag.Comment
        if ($comment) {
            $x = [Math]::Max($script:commentX, $filesRect.Right + 24)
            # Warnings ("also in", "duplicated") are drawn in red: a red mark first, so it is visible
            # even when the text does not fit, and the warning text after the comment
            $warnAt = $comment.IndexOf([char]0x26A0)
            $main = if ($warnAt -gt 0) { $comment.Substring(0, $warnAt).TrimEnd() } else { $comment }
            $red = [Drawing.Color]::FromArgb(190, 0, 0)
            if ($warnAt -gt 0) {
                $markRect = New-Object Drawing.Rectangle(($x - 20), $e.Bounds.Y, 20, $e.Bounds.Height)
                [Windows.Forms.TextRenderer]::DrawText($e.Graphics, [string][char]0x26A0, $tree.Font, $markRect, $red, $flags)
            }
            $rect = New-Object Drawing.Rectangle($x, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $x), $e.Bounds.Height)
            $color = if ($studyBrush) { [Drawing.Color]::Black }
                     elseif ($comment -like 'Counting*' -or $comment -like 'No DICOM*') { [Drawing.Color]::Gray }
                     else { [Drawing.Color]::FromArgb(0, 95, 170) }
            [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $main, $tree.Font, $rect, $color, $flags -bor [Windows.Forms.TextFormatFlags]::EndEllipsis)
            if ($warnAt -gt 0) {
                $mainWidth = [Windows.Forms.TextRenderer]::MeasureText($e.Graphics, $main, $tree.Font, $rect.Size, $flags).Width
                $wx = $x + $mainWidth + 6
                $warnRect = New-Object Drawing.Rectangle($wx, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $wx), $e.Bounds.Height)
                [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $comment.Substring($warnAt), $tree.Font, $warnRect, $red, $flags -bor [Windows.Forms.TextFormatFlags]::EndEllipsis)
            }
        }
    })
    $tree.add_NodeMouseDoubleClick({
        param($sender, $e)
        Start-Process explorer.exe ('"{0}"' -f $e.Node.Tag.Path)
    })
    $form.add_KeyDown({
        param($sender, $e)
        if ($e.KeyCode -eq 'F5') { Update-Tree; $e.Handled = $true }
    })
    $form.add_Shown({ $form.Activate() })

    # Live updates: a count writes desktop.ini folder by folder, and folders can be created,
    # deleted or renamed meanwhile. The watchers only collect what changed (SynchronizingObject
    # delivers their events on the window thread, as PowerShell requires); the timer applies it
    # in batches, since each comment is written in several steps, and repaints only those rows.
    $script:dirty = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:dirtyParents = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:dirtyFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:needsRebuild = $false

    # Files created, deleted or renamed: that folder is recounted
    $fileWatcher = New-Object IO.FileSystemWatcher($root)
    $fileWatcher.IncludeSubdirectories = $true
    $fileWatcher.NotifyFilter = [IO.NotifyFilters]::FileName
    $fileWatcher.InternalBufferSize = 65536
    $fileWatcher.SynchronizingObject = $form
    $onFileChange = {
        param($sender, $e)
        if ([IO.Path]::GetFileName($e.FullPath) -ne 'desktop.ini') {
            [void]$script:dirtyFiles.Add([IO.Path]::GetDirectoryName($e.FullPath))
        }
    }
    $fileWatcher.add_Created($onFileChange)
    $fileWatcher.add_Deleted($onFileChange)
    $fileWatcher.add_Renamed($onFileChange)
    $fileWatcher.add_Error({ $script:needsRebuild = $true })

    $dirWatcher = New-Object IO.FileSystemWatcher($root)
    $dirWatcher.IncludeSubdirectories = $true
    $dirWatcher.NotifyFilter = [IO.NotifyFilters]::DirectoryName
    $dirWatcher.InternalBufferSize = 65536
    $dirWatcher.SynchronizingObject = $form
    $onDirChange = {
        param($sender, $e)
        [void]$script:dirtyParents.Add([IO.Path]::GetDirectoryName($e.FullPath))
        if ($e -is [IO.RenamedEventArgs]) { [void]$script:dirtyParents.Add([IO.Path]::GetDirectoryName($e.OldFullPath)) }
    }
    $dirWatcher.add_Created($onDirChange)
    $dirWatcher.add_Deleted($onDirChange)
    $dirWatcher.add_Renamed($onDirChange)
    # Buffer overflow (too many changes at once): rebuild the whole tree
    $dirWatcher.add_Error({ $script:needsRebuild = $true })

    $watcher = New-Object IO.FileSystemWatcher($root, 'desktop.ini')
    $watcher.IncludeSubdirectories = $true
    $watcher.NotifyFilter = [IO.NotifyFilters]'FileName, LastWrite, Size'
    $watcher.InternalBufferSize = 65536
    $watcher.SynchronizingObject = $form
    $onChange = { param($sender, $e) [void]$script:dirty.Add([IO.Path]::GetDirectoryName($e.FullPath)) }
    $watcher.add_Changed($onChange)
    $watcher.add_Created($onChange)
    $watcher.add_Deleted($onChange)
    $watcher.add_Renamed($onChange)
    # Buffer overflow (too many changes at once): re-read every folder
    $watcher.add_Error({ foreach ($p in @($script:nodeByPath.Keys)) { [void]$script:dirty.Add($p) } })

    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 300
    $timer.add_Tick({
        if ($script:problemsCheckAt -and (Get-Date) -gt $script:problemsCheckAt) {
            $script:problemsCheckAt = $null
            Update-ProblemsButton
        }
        if ($script:needsRebuild) {
            $script:needsRebuild = $false
            $script:dirtyParents.Clear()
            $script:dirtyFiles.Clear()
            Update-Tree
        }
        if ($script:dirtyParents.Count) {
            # Parents first, so a new folder inside another new folder already has its place
            $parents = @($script:dirtyParents) | Sort-Object Length
            $script:dirtyParents.Clear()
            $tree.BeginUpdate()
            foreach ($p in $parents) {
                $node = $script:nodeByPath[$p]
                if ($node) { Sync-Children $node }
            }
            $tree.EndUpdate()
            Update-StudyColors
            Update-Status
        }
        if ($script:dirtyFiles.Count) {
            $dirs = @($script:dirtyFiles)
            $script:dirtyFiles.Clear()
            foreach ($d in $dirs) {
                $node = $script:nodeByPath[$d]
                if (-not $node) { continue }     # new folder: counted when its node is added
                $direct = Get-DirectFileCount $d
                $delta = $direct - $node.Tag.DirectFiles
                $node.Tag.DirectFiles = $direct
                Add-FileDelta $node $delta
            }
            Update-Status
        }
        if ($script:dirty.Count -eq 0) { return }
        $changed = @($script:dirty)
        $script:dirty.Clear()
        foreach ($dir in $changed) {
            $node = $script:nodeByPath[$dir]
            if (-not $node) { continue }
            $comment = Get-FolderComment $dir
            if ($comment -ne $node.Tag.Comment) {
                $node.Tag.Comment = $comment
                $node.Tag.StudyUid = Get-StudyUid $comment
                $node.ToolTipText = $comment
                Repaint-Row $node
            }
        }
        Update-StudyColors
        Update-Status
        # The root is the last folder written by a count; its report is saved right after,
        # so the Problems button is refreshed a moment later
        if ($changed -contains $root) { $script:problemsCheckAt = (Get-Date).AddSeconds(2) }
    })

    Update-Tree
    $watcher.EnableRaisingEvents = $true
    $dirWatcher.EnableRaisingEvents = $true
    $fileWatcher.EnableRaisingEvents = $true
    $timer.Start()
    [void]$form.ShowDialog()
    $timer.Stop()
    $watcher.Dispose()
    $dirWatcher.Dispose()
    $fileWatcher.Dispose()
}

if ($RestoreQuarantine) {
    $r = Restore-Quarantine $RestoreQuarantine.TrimEnd('\')
    Write-Host ("Restored {0:N0} files ({1:N0} skipped: already back or missing)" -f $r.Restored, $r.Skipped)
    return
}

if ($Watch) {
    $isFirst = $false
    $mutex = New-Object Threading.Mutex($true, 'Local\DicomStudyCountWatcher', [ref]$isFirst)
    if (-not $isFirst) { return }     # another instance is already running

    $shell = New-Object -ComObject Shell.Application
    $handled = @{}      # "hwnd|path" already checked; every tab is a separate entry
    $loops = 0
    while ($true) {
        $seen = @{}
        try {
            # ShellWindows does not enumerate reliably with foreach in PowerShell: iterate by index
            $windows = $shell.Windows()
            for ($i = 0; $i -lt $windows.Count; $i++) {
                try {
                    $window = $windows.Item($i)
                    $path = $window.Document.Folder.Self.Path
                    $key = "$($window.HWND)|$path"
                    $seen[$key] = $true
                    if ($handled.ContainsKey($key)) { continue }
                    if ([IO.Path]::IsPathRooted($path) -and (Test-HasDicomComments $path)) {
                        [void][DicomStudyCount.ExplorerColumns]::ShowCommentColumn($window)
                    }
                    $handled[$key] = $true
                }
                catch { }   # view still loading: retried on the next pass
            }
        }
        catch { }
        # Forget what is no longer open, so it is checked again when returning to that folder
        foreach ($k in @($handled.Keys)) { if (-not $seen.ContainsKey($k)) { $handled.Remove($k) } }
        if ((++$loops % 600) -eq 0) { [GC]::Collect() }
        Start-Sleep -Milliseconds 500
    }
}

try {
    # From the context menu a drive root arrives as 'D:"' because of the trailing backslash escaping
    $Path = $Path.Trim().Trim('"')
    if ($Path -match '^[A-Za-z]:$') { $Path += '\' }
    $root = (Resolve-Path -LiteralPath $Path).ProviderPath
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Not a folder: $root" }

    if ($ShowTree) {
        Show-DicomTree $root
        return
    }

    if ($ClearComments) {
        $cleared = 0
        foreach ($ini in Get-DesktopIniFiles $root) { if ([DicomStudyCount.FolderComments]::Clear($ini.DirectoryName)) { $cleared++ } }
        Write-Host "Comments removed from $cleared folders in $root"
        return
    }

    if ($WriteComments) { Show-CommentColumn $root }

    $progress = New-Object DicomStudyCount.ScanProgress
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $task = [DicomStudyCount.Scanner]::StartScan($root, [Environment]::ProcessorCount, $progress, [bool]$WriteComments, $indexDir)
    $countingFitted = $false
    while (-not $task.Wait(200)) {
        Write-Progress -Activity "Reading DICOM in $root" `
            -Status ('{0:N0} of {1:N0} files read - {2:N0} studies found' -f $progress.Files, $progress.TotalFiles, $progress.Studies)
        # Fit the width to the "Counting..." text as soon as Explorer shows it
        if ($WriteComments -and -not $countingFitted -and $progress.CountingShown) {
            Show-CommentColumn $root
            $countingFitted = $true
        }
    }
    Write-Progress -Activity "Reading DICOM in $root" -Completed
    $result = $task.Result
    $c = $result.Counts
    $sw.Stop()

    try { Save-ProblemsReport $root $result }
    catch { Write-Host "Could not save the problems report: $($_.Exception.Message)" -ForegroundColor Yellow }

    if ($WriteComments) {
        # The texts were "Counting..." at the start; now the width is refitted to the final ones
        # (after a short pause so Explorer has refreshed the values)
        Start-Sleep -Milliseconds 700
        Show-CommentColumn $root
    }

    Write-Host ''
    Write-Host "Folder: $root"
    Write-Host ''
    Write-Host ('  DISTINCT STUDIES:       {0:N0}' -f $result.Studies.Count) -ForegroundColor Green
    Write-Host ''
    Write-Host ('  DICOM files:            {0:N0}' -f $c.Dicom)
    if ($c.NoStudy -gt 0) { Write-Host ('  DICOM without StudyUID: {0:N0}' -f $c.NoStudy) }
    Write-Host ('  Non-DICOM files:        {0:N0}' -f $c.NotDicom)
    if ($c.Errors -gt 0) { Write-Host ('  Read errors:            {0:N0}' -f $c.Errors) -ForegroundColor Yellow }
    if ($c.InaccessibleDirs.Count -gt 0) {
        Write-Host ('  Inaccessible folders:   {0:N0}' -f $c.InaccessibleDirs.Count) -ForegroundColor Yellow
        $c.InaccessibleDirs | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" -ForegroundColor Yellow }
    }
    Write-Host ('  Time:                   {0:N1} s' -f $sw.Elapsed.TotalSeconds)

    if ($Detail -and $result.Studies.Count -gt 0) {
        $result.Studies |
            Format-Table StudyDate, Modalities, PatientID, StudyDescription, Series, Files, StudyInstanceUID -AutoSize |
            Out-String -Width 400 | Write-Host
    }

    if ($Csv) {
        $result.Studies | Export-Csv -LiteralPath $Csv -NoTypeInformation -Encoding UTF8 -Delimiter ';'
        Write-Host "CSV saved to $Csv"
    }

    $relative = { param($p) if ($p.Length -le $root.Length) { '(root)' } else { $p.Substring($root.Length).TrimStart('\') } }
    if ($result.Splits.Count) {
        Write-Host ''
        Write-Host ("Studies split across folders: {0}" -f $result.Splits.Count) -ForegroundColor Yellow
        foreach ($s in $result.Splits) {
            Write-Host ("  [{0}] {1}" -f $s.Tag, $s.StudyInstanceUID)
            foreach ($folder in $s.Folders) { Write-Host ("      {0}" -f (& $relative $folder)) }
        }
    }
    if ($result.Duplicates.Count) {
        Write-Host ''
        Write-Host ("Duplicated files (same StudyInstanceUID and SOPInstanceUID): {0:N0} files in {1:N0} folder sets" -f $result.DuplicatedFiles, $result.Duplicates.Count) -ForegroundColor Yellow
        foreach ($d in ($result.Duplicates | Select-Object -First 10)) {
            Write-Host ("  {0:N0} files ({1:N0} byte-identical copies, {2:N0} with different contents):" -f $d.Files, $d.Identical, $d.Different)
            foreach ($folder in $d.Folders) { Write-Host ("      {0}" -f (& $relative $folder)) }
        }
        if ($result.Duplicates.Count -gt 10) { Write-Host ("  ... and {0} more sets" -f ($result.Duplicates.Count - 10)) }
    }
    if ($result.Incomplete.Count) {
        Write-Host ''
        Write-Host ("Incomplete series: {0:N0}" -f $result.Incomplete.Count) -ForegroundColor Yellow
        foreach ($s in ($result.Incomplete | Select-Object -First 10)) {
            Write-Host ("  [{0}] {1}: {2} of {3}, missing {4}" -f $s.Tag, $s.Series, $s.Have, $s.Expected, $s.Missing)
            Write-Host ("      {0}" -f (& $relative $s.Folder))
        }
        if ($result.Incomplete.Count -gt 10) { Write-Host ("  ... and {0} more" -f ($result.Incomplete.Count - 10)) }
    }
    if ($result.External.Count) {
        $externalFiles = ($result.External | Measure-Object -Property Files -Sum).Sum
        Write-Host ''
        Write-Host ("Also in other counted locations: {0:N0} files in {1:N0} folder pairs" -f $externalFiles, $result.External.Count) -ForegroundColor Yellow
        foreach ($x in ($result.External | Select-Object -First 10)) {
            Write-Host ("  {0:N0} files ({1:N0} byte-identical, {2:N0} with different contents):" -f $x.Files, $x.Identical, $x.Different)
            Write-Host ("      {0}" -f (& $relative $x.Folder))
            Write-Host ("      {0}" -f $x.OtherFolder)
        }
        if ($result.External.Count -gt 10) { Write-Host ("  ... and {0} more" -f ($result.External.Count - 10)) }
    }

    if ($WriteComments) {
        $withStudies = @($result.Folders | Where-Object { $_.Studies -gt 0 }).Count
        Write-Host ''
        Write-Host ("Comments written to {0:N0} folders with DICOM" -f $withStudies) -ForegroundColor Green
        if ($c.CommentErrors -gt 0) { Write-Host ("Folders that could not be written: {0:N0}" -f $c.CommentErrors) -ForegroundColor Yellow }
    }
}
catch {
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    if ($Pause) {
        Write-Host ''
        Write-Host 'Press any key to close...'
        [void][Console]::ReadKey($true)
    }
}
