// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the comments of the folders (desktop.ini InfoTip): what the count writes, and reads back

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
}
