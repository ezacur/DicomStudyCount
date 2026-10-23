<#
.SYNOPSIS
    Tests DicomIcons.dll outside Explorer: loads it in this process (no registration), asks it for the
    icon of every file given and saves the icons drawn as PNG, plus a sheet with all the colors.
.EXAMPLE
    .\Test-DicomIcons.ps1 -Files (Get-ChildItem D:\DICOM_DEMO -Recurse -File) -Out C:\temp\icons
#>
param(
    [Parameter(Mandatory = $true)] [string[]]$Files,
    [Parameter(Mandatory = $true)] [string]$Out,
    [string]$Dll
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults
if (-not $Dll) { $Dll = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'bin\DicomIcons.dll' }
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Runtime.InteropServices; using System.Text;
public static class IconTest {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("user32.dll")] static extern bool DestroyIcon(IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetClassObject(ref Guid clsid, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IClassFactory f);

    [ComImport, Guid("00000001-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IClassFactory {
        void CreateInstance([MarshalAs(UnmanagedType.IUnknown)] object outer, ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object obj);
        void LockServer(bool lck);
    }
    [ComImport, Guid("0000010b-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPersistFile {
        void GetClassID(out Guid clsid); [PreserveSig] int IsDirty();
        void Load([MarshalAs(UnmanagedType.LPWStr)] string file, uint mode);
        void Save(IntPtr f, bool remember); void SaveCompleted(IntPtr f); void GetCurFile(out IntPtr f);
    }
    [ComImport, Guid("000214FA-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IExtractIconW {
        [PreserveSig] int GetIconLocation(uint flags, StringBuilder file, uint cch, out int index, out uint outFlags);
        [PreserveSig] int Extract([MarshalAs(UnmanagedType.LPWStr)] string file, uint index, out IntPtr large, out IntPtr small, uint sizes);
    }

    static IClassFactory factory;
    public static void Load(string dll) {
        IntPtr m = LoadLibraryW(dll);
        if (m == IntPtr.Zero) throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
        var get = (GetClassObject)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "DllGetClassObject"), typeof(GetClassObject));
        Guid clsid = new Guid("F4798986-2EEA-49F6-8082-2FC552CE691B"), iid = typeof(IClassFactory).GUID;
        int hr = get(ref clsid, ref iid, out factory);
        if (hr != 0) throw new Exception("DllGetClassObject: 0x" + hr.ToString("X8"));
    }

    // Location and flags the handler gives for the file
    public static string Location(string path, out uint flags) {
        Guid iid = typeof(IPersistFile).GUID; object o;
        factory.CreateInstance(null, ref iid, out o);
        ((IPersistFile)o).Load(path, 0);
        var sb = new StringBuilder(260); int index;
        // GIL_ASYNC first, as Explorer does: the handler must ask to be called again
        int pending = ((IExtractIconW)o).GetIconLocation(0x20, sb, 260, out index, out flags);
        if (pending != unchecked((int)0x8000000A)) return "(GIL_ASYNC did not return E_PENDING: 0x" + pending.ToString("X8") + ")";
        int hr = ((IExtractIconW)o).GetIconLocation(0, sb, 260, out index, out flags);
        return hr == 0 ? sb.ToString() + "," + index : "(0x" + hr.ToString("X8") + ")";
    }

    // The icon drawn for a location key, as a bitmap with its alpha
    public static Bitmap Draw(string key, int size) {
        Guid iid = typeof(IExtractIconW).GUID; object o;
        factory.CreateInstance(null, ref iid, out o);
        IntPtr large, small;
        int hr = ((IExtractIconW)o).Extract(key, 0, out large, out small, (uint)(size | (16 << 16)));
        if (hr != 0 || large == IntPtr.Zero) return null;
        var bmp = new Bitmap(Icon.FromHandle(large).ToBitmap());
        DestroyIcon(large); if (small != IntPtr.Zero) DestroyIcon(small);
        return bmp;
    }
}
'@
[IconTest]::Load($Dll)
New-Item -ItemType Directory -Force $Out | Out-Null

foreach ($f in $Files) {
    $flags = [uint32]0
    $loc = [IconTest]::Location($f, [ref]$flags)
    '{0,-60} {1}  flags 0x{2:X}' -f ($f.Substring([Math]::Max(0, $f.Length - 60))), $loc, $flags
}

# Sheet: every color with a few numbers, at 16, 24, 32 and 48 px, on a light and a dark background
Add-Type -AssemblyName System.Drawing
$labels = '1', '7', '12', '301', '1001', '?'
$sizes = 16, 24, 32, 48
$zoom = 2; $cellW = 56 * $zoom; $rows = 22
$sheet = New-Object Drawing.Bitmap(($labels.Count * $sizes.Count * 52 * 2 + 40), ($rows * 52))
$g = [Drawing.Graphics]::FromImage($sheet); $g.InterpolationMode = 'NearestNeighbor'; $g.Clear([Drawing.Color]::White)
$dark = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(32, 32, 32))
for ($c = 0; $c -lt $rows; $c++) {
    $x = 0
    foreach ($bg in 'light', 'dark') {
        if ($bg -eq 'dark') { $g.FillRectangle($dark, $x, $c * 52, $labels.Count * $sizes.Count * 52, 52) }
        foreach ($s in $sizes) {
            foreach ($l in $labels) {
                $b = [IconTest]::Draw("DicomStudyCount|v2|$c|$l", $s)
                if ($b) { $g.DrawImage($b, $x + 2, $c * 52 + 2, $s, $s); $b.Dispose() }
                $x += 52
            }
        }
        $x += 40
    }
}
$sheet.Save((Join-Path $Out 'sheet.png'))
# The 16 px row zoomed, the size of the Details view
$z = New-Object Drawing.Bitmap(($labels.Count * 20 * 4), ($rows * 20 * 4))
$gz = [Drawing.Graphics]::FromImage($z); $gz.InterpolationMode = 'NearestNeighbor'; $gz.PixelOffsetMode = 'Half'; $gz.Clear([Drawing.Color]::White)
for ($c = 0; $c -lt $rows; $c++) {
    for ($i = 0; $i -lt $labels.Count; $i++) {
        $b = [IconTest]::Draw("DicomStudyCount|v2|$c|$($labels[$i])", 16)
        if ($b) { $gz.DrawImage($b, $i * 80 + 8, $c * 80 + 8, 64, 64); $b.Dispose() }
    }
}
$z.Save((Join-Path $Out 'sheet16-zoom.png'))
"sheets saved to $Out"
