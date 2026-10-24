<#
.SYNOPSIS
    Tests the preview of bin\DicomShell.dll outside Explorer: loads it in this process (no
    registration), shows each file in a window as the preview pane would, and saves a screenshot.
.EXAMPLE
    .\Test-DicomPreview.ps1 -Files D:\DICOM\IM0001 -Out C:\temp\previews
#>
param(
    [Parameter(Mandatory = $true)] [string[]]$Files,
    [Parameter(Mandatory = $true)] [string]$Out,
    [string]$Dll,
    [int]$Width = 700,
    [int]$Height = 900
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults
if (-not $Dll) { $Dll = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'bin\DicomShell.dll' }
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms -TypeDefinition @'
using System; using System.Drawing; using System.Runtime.InteropServices;
public static class PreviewTest {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryW(string p);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)] static extern int SHCreateStreamOnFileEx(string file, uint mode, uint attrs, bool create, IntPtr template, out IntPtr stream);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetClassObject(ref Guid c, ref Guid i, [MarshalAs(UnmanagedType.Interface)] out IClassFactory f);
    [ComImport, Guid("00000001-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IClassFactory { void CreateInstance([MarshalAs(UnmanagedType.IUnknown)] object o, ref Guid i, [MarshalAs(UnmanagedType.IUnknown)] out object r); void LockServer(bool l); }
    [ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IInitializeWithStream { void Initialize(IntPtr stream, uint mode); }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [ComImport, Guid("8895b1c6-b41f-4c1c-a562-0d564250836f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPreviewHandler {
        void SetWindow(IntPtr hwnd, ref RECT rc); void SetRect(ref RECT rc); void DoPreview(); void Unload();
        void SetFocus(); void QueryFocus(out IntPtr h); [PreserveSig] int TranslateAccelerator(IntPtr msg);
    }
    static IClassFactory f;
    public static void Load(string dll) {
        var get = (GetClassObject)Marshal.GetDelegateForFunctionPointer(GetProcAddress(LoadLibraryW(dll), "DllGetClassObject"), typeof(GetClassObject));
        Guid c = new Guid("F3E184D5-95DF-4BE9-9929-32FF8A59CD55"), i = typeof(IClassFactory).GUID;
        Marshal.ThrowExceptionForHR(get(ref c, ref i, out f));
    }
    // Shows the file in the panel, lets it paint, saves a picture of it
    public static void Show(string file, System.Windows.Forms.Control panel, string png) {
        Guid i = typeof(IPreviewHandler).GUID; object o;
        f.CreateInstance(null, ref i, out o);
        IntPtr stream;
        Marshal.ThrowExceptionForHR(SHCreateStreamOnFileEx(file, 0x20 /* STGM_SHARE_DENY_WRITE */, 0, false, IntPtr.Zero, out stream));
        ((IInitializeWithStream)o).Initialize(stream, 0);
        Marshal.Release(stream);
        var ph = (IPreviewHandler)o;
        var rc = new RECT { Right = panel.ClientSize.Width, Bottom = panel.ClientSize.Height };
        ph.SetWindow(panel.Handle, ref rc);
        ph.DoPreview();
        for (int k = 0; k < 20; k++) { System.Windows.Forms.Application.DoEvents(); System.Threading.Thread.Sleep(25); }
        using (var bmp = new Bitmap(panel.Width, panel.Height))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc(); PrintWindow(panel.Handle, dc, 0); g.ReleaseHdc(dc); bmp.Save(png);
        }
        ph.Unload();
        Marshal.ReleaseComObject(o);
    }
}
'@
[PreviewTest]::Load($Dll)
New-Item -ItemType Directory -Force $Out | Out-Null
$form = New-Object Windows.Forms.Form
$form.ClientSize = New-Object Drawing.Size($Width, $Height)
$form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(-3000, 0)    # off screen
$form.ShowInTaskbar = $false
$panel = New-Object Windows.Forms.Panel
$panel.Dock = 'Fill'
$form.Controls.Add($panel)
$form.Show()
$n = 0
foreach ($file in $Files) {
    $n++
    $png = Join-Path $Out ('{0:D2}_{1}.png' -f $n, ([IO.Path]::GetFileName($file) -replace '[^\w.-]', '_'))
    try { [PreviewTest]::Show($file, $panel, $png); "ok    $file" }
    catch { "ERROR $file : $($_.Exception.Message)" }
}
$form.Close()
