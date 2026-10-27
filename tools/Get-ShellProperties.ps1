<#
.SYNOPSIS
    Prints properties of files as Explorer sees them (through the property system, so through the
    registered property handlers): to check the DICOM property handler.
.EXAMPLE
    .\Get-ShellProperties.ps1 -Files D:\DICOM\IM0001 -Properties System.Title, Explorer4Dicoms.SeriesNumber
#>
param(
    [Parameter(Mandatory = $true)] [string[]]$Files,
    [string[]]$Properties = @('System.Title')
)
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class ShellProps {
    [StructLayout(LayoutKind.Sequential)] public struct PROPERTYKEY { public Guid fmtid; public uint pid; }
    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        void GetCount(out uint c); void GetAt(uint i, out PROPERTYKEY k);
        void GetValue(ref PROPERTYKEY k, IntPtr pv); void SetValue(ref PROPERTYKEY k, IntPtr pv); void Commit();
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern int SHGetPropertyStoreFromParsingName(string path, IntPtr bc, int flags, ref Guid iid, out IPropertyStore ps);
    [DllImport("propsys.dll", CharSet = CharSet.Unicode)] static extern int PSGetPropertyKeyFromName(string name, out PROPERTYKEY k);
    [DllImport("propsys.dll", CharSet = CharSet.Unicode)] static extern int PropVariantToStringAlloc(IntPtr pv, out IntPtr s);
    [DllImport("ole32.dll")] static extern int PropVariantClear(IntPtr pv);

    public static string Get(string path, string name) {
        PROPERTYKEY k;
        if (PSGetPropertyKeyFromName(name, out k) != 0) return "(unknown property)";
        Guid iid = typeof(IPropertyStore).GUID; IPropertyStore ps;
        int hr = SHGetPropertyStoreFromParsingName(path, IntPtr.Zero, 0, ref iid, out ps);
        if (hr != 0) return "(error 0x" + hr.ToString("X8") + ")";
        IntPtr pv = Marshal.AllocCoTaskMem(32);
        try {
            for (int b = 0; b < 32; b++) Marshal.WriteByte(pv, b, 0);
            ps.GetValue(ref k, pv);
            if (Marshal.ReadInt16(pv) == 0) return "(empty)";
            IntPtr s; if (PropVariantToStringAlloc(pv, out s) != 0) return "(vt " + Marshal.ReadInt16(pv) + ")";
            string r = Marshal.PtrToStringUni(s); Marshal.FreeCoTaskMem(s); return r;
        } finally { PropVariantClear(pv); Marshal.FreeCoTaskMem(pv); Marshal.ReleaseComObject(ps); }
    }
}
'@
foreach ($f in $Files) {
    $f
    foreach ($p in $Properties) { '   {0,-34} {1}' -f $p, [ShellProps]::Get($f, $p) }
}
