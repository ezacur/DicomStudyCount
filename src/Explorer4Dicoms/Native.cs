// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the Win32 calls of the C# part (folder settings, DPI)

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
}
