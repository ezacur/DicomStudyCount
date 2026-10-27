<#
.SYNOPSIS
    Tests the preview of bin\DicomShell.dll outside Explorer: loads it in this process (no
    registration), shows each file in a window as the preview pane would, and saves a screenshot.
    -Lister: as Total Commander's Lister would instead (the plugin's ListLoadW and ListCloseWindow).
.EXAMPLE
    .\Test-DicomPreview.ps1 -Files D:\DICOM\IM0001 -Out C:\temp\previews
    .\Test-DicomPreview.ps1 -Files D:\DICOM\cine.dcm -Out C:\temp\previews -Keys Right, End, Home   # a picture after each
    .\Test-DicomPreview.ps1 -Files D:\DICOM\IM0001 -Out C:\temp\previews -Lister
#>
param(
    [Parameter(Mandatory = $true)] [string[]]$Files,
    [Parameter(Mandatory = $true)] [string]$Out,
    [string]$Dll,
    [int]$Width = 700,
    [int]$Height = 900,
    [switch]$Dark,         # the colors of Explorer in dark mode
    [int]$Settle = 500,    # ms it paints before its picture (more: what it reads in the background, the segmentations)
    [string[]]$Keys,      # keys pressed in it after that (Right, End...: System.Windows.Forms.Keys), a picture after each
    [switch]$Lister        # as Total Commander's Lister shows it (the plugin), not as the preview pane
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
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern int SHCreateItemFromParsingName(string path, IntPtr bc, ref Guid riid, out IntPtr item);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowExW(IntPtr p, IntPtr after, string c, string w);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetClassObject(ref Guid c, ref Guid i, [MarshalAs(UnmanagedType.Interface)] out IClassFactory f);
    [ComImport, Guid("00000001-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IClassFactory { void CreateInstance([MarshalAs(UnmanagedType.IUnknown)] object o, ref Guid i, [MarshalAs(UnmanagedType.IUnknown)] out object r); void LockServer(bool l); }
    [ComImport, Guid("7f73be3f-fb79-493c-a6c7-7ee14e245841"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IInitializeWithItem { void Initialize(IntPtr item, uint mode); }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [ComImport, Guid("8895b1c6-b41f-4c1c-a562-0d564250836f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPreviewHandler {
        void SetWindow(IntPtr hwnd, ref RECT rc); void SetRect(ref RECT rc); void DoPreview(); void Unload();
        void SetFocus(); void QueryFocus(out IntPtr h); [PreserveSig] int TranslateAccelerator(IntPtr msg);
    }
    static IClassFactory f;
    // The Lister plugin: its window in the panel, as Total Commander asks for it
    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)] delegate IntPtr ListLoadW(IntPtr parent, string file, int flags);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate void ListCloseWindow(IntPtr window);
    static ListLoadW listLoad;
    static ListCloseWindow listClose;
    public static void LoadLister(string dll) {
        IntPtr m = LoadLibraryW(dll);
        listLoad = (ListLoadW)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "ListLoadW"), typeof(ListLoadW));
        listClose = (ListCloseWindow)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "ListCloseWindow"), typeof(ListCloseWindow));
    }
    public static void List(string file, System.Windows.Forms.Control panel, string png, int settle, int[] keys) {
        IntPtr h = listLoad(panel.Handle, file, 0);
        if (h == IntPtr.Zero) throw new Exception("the plugin did not take it");
        Settle(settle);
        Save(panel, png);
        for (int k = 0; k < keys.Length; k++) {
            // (to the preview's window in the plugin's)
            PostMessageW(FindWindowExW(h, IntPtr.Zero, null, null), 0x0100, (IntPtr)keys[k], IntPtr.Zero);
            Settle(settle);
            Save(panel, png.Substring(0, png.Length - 4) + "_key" + (k + 1) + ".png");
        }
        listClose(h);
    }
    public static void Load(string dll) {
        var get = (GetClassObject)Marshal.GetDelegateForFunctionPointer(GetProcAddress(LoadLibraryW(dll), "DllGetClassObject"), typeof(GetClassObject));
        Guid c = new Guid("F3E184D5-95DF-4BE9-9929-32FF8A59CD55"), i = typeof(IClassFactory).GUID;
        Marshal.ThrowExceptionForHR(get(ref c, ref i, out f));
    }
    // Shows the file in the panel, lets it paint, saves a picture of it
    [ComImport, Guid("196bf9a5-b346-4ef0-aa1e-5dcdb76768b1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPreviewHandlerVisuals { void SetBackgroundColor(uint c); void SetFont(IntPtr f); void SetTextColor(uint c); }
    static void Settle(int ms) { for (int k = 0; k < ms / 25; k++) { System.Windows.Forms.Application.DoEvents(); System.Threading.Thread.Sleep(25); } }
    static void Save(System.Windows.Forms.Control panel, string png) {
        using (var bmp = new Bitmap(panel.Width, panel.Height))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc(); PrintWindow(panel.Handle, dc, 0); g.ReleaseHdc(dc); bmp.Save(png);
        }
    }
    public static void Show(string file, System.Windows.Forms.Control panel, string png, bool dark, int settle, int[] keys) {
        Guid i = typeof(IPreviewHandler).GUID; object o;
        f.CreateInstance(null, ref i, out o);
        IntPtr item; Guid itemId = new Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe");    // IShellItem
        Marshal.ThrowExceptionForHR(SHCreateItemFromParsingName(file, IntPtr.Zero, ref itemId, out item));
        ((IInitializeWithItem)o).Initialize(item, 0);
        Marshal.Release(item);
        var ph = (IPreviewHandler)o;
        var rc = new RECT { Right = panel.ClientSize.Width, Bottom = panel.ClientSize.Height };
        if (dark) { ((IPreviewHandlerVisuals)o).SetBackgroundColor(0x202020); ((IPreviewHandlerVisuals)o).SetTextColor(0xFFFFFF); panel.BackColor = Color.FromArgb(32, 32, 32); }
        ph.SetWindow(panel.Handle, ref rc);
        ph.DoPreview();
        Settle(settle);
        Save(panel, png);
        for (int k = 0; k < keys.Length; k++) {
            // (WM_KEYDOWN to its window, the one in the panel: the form, off screen, has no focus)
            PostMessageW(FindWindowExW(panel.Handle, IntPtr.Zero, null, null), 0x0100, (IntPtr)keys[k], IntPtr.Zero);
            Settle(settle);
            Save(panel, png.Substring(0, png.Length - 4) + "_key" + (k + 1) + ".png");
        }
        ph.Unload();
        Marshal.ReleaseComObject(o);
    }
}
'@
if ($Lister) { [PreviewTest]::LoadLister($Dll) } else { [PreviewTest]::Load($Dll) }
New-Item -ItemType Directory -Force $Out | Out-Null
$form = New-Object Windows.Forms.Form
$form.ClientSize = New-Object Drawing.Size($Width, $Height)
$form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(-3000, 0)    # off screen
$form.ShowInTaskbar = $false
$panel = New-Object Windows.Forms.Panel
$panel.Dock = 'Fill'
$form.Controls.Add($panel)
$form.Show()
$keyCodes = [int[]]@(foreach ($k in $Keys) { [int][Windows.Forms.Keys]$k })
$n = 0
foreach ($file in $Files) {
    $n++
    $png = Join-Path $Out ('{0:D2}_{1}.png' -f $n, ([IO.Path]::GetFileName($file) -replace '[^\w.-]', '_'))
    try {
        if ($Lister) { [PreviewTest]::List($file, $panel, $png, $Settle, $keyCodes) }
        else { [PreviewTest]::Show($file, $panel, $png, $Dark, $Settle, $keyCodes) }
        "ok    $file"
    }
    catch { "ERROR $file : $($_.Exception.Message)" }
}
$form.Close()
