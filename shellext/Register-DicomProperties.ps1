<#
.SYNOPSIS
    Registers the DICOM property handler of DicomIcons.dll for the whole machine (it needs administrator
    rights: Windows reads property handlers only from HKLM). -Uninstall removes it.
.NOTES
    The DLL is copied to %ProgramFiles%\DicomStudyCount, which only administrators can change: Explorer
    and the search indexer load it. Its name carries its hash, since a loaded DLL cannot be overwritten.
    An extension that already has a property handler is left alone.
#>
param(
    [string]$Dll,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
if (-not (New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator rights are needed (run it elevated)'
}
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults
if (-not $Dll) { $Dll = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'bin\DicomIcons.dll' }

$clsid = '{21329A2B-09B3-4250-9103-9267877321EB}'
$handlers = 'SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers'
$state = 'SOFTWARE\DicomStudyCount\PropertyHandler'      # what was registered, to undo only that
$extensions = '.', '.dcm'                                   # '.': files without extension
$hklm = [Microsoft.Win32.Registry]::LocalMachine
$dir = Join-Path $env:ProgramFiles 'DicomStudyCount'
# The columns (names, types) of the properties: a schema registered from its installed copy
$schemaSource = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'DicomStudyCount.propdesc'
$schema = Join-Path $dir 'DicomStudyCount.propdesc'
Add-Type -Namespace DicomPropsSetup -Name PropSys -MemberDefinition '
    [DllImport("propsys.dll", CharSet = CharSet.Unicode)] public static extern int PSRegisterPropertySchema(string path);
    [DllImport("propsys.dll", CharSet = CharSet.Unicode)] public static extern int PSUnregisterPropertySchema(string path);
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);'

function Remove-Key([string]$path) { if ($hklm.OpenSubKey($path)) { $hklm.DeleteSubKeyTree($path) } }

if (Test-Path -LiteralPath $schema) { [void][DicomPropsSetup.PropSys]::PSUnregisterPropertySchema($schema) }

if ($Uninstall) {
    if (Test-Path -LiteralPath $schema) { Remove-Item -LiteralPath $schema -Force }
    $s = $hklm.OpenSubKey($state)
    foreach ($ext in $extensions) {
        $k = $hklm.OpenSubKey("$handlers\$ext")
        if ($s -and $s.GetValue("Owns$ext") -eq 1 -and $k -and $k.GetValue('') -eq $clsid) { Remove-Key "$handlers\$ext" }
    }
    Remove-Key "SOFTWARE\Classes\CLSID\$clsid"
    Remove-Key $state
    Write-Host 'DICOM properties unregistered (Explorer frees the DLL when it restarts).'
    return
}

if (-not (Test-Path -LiteralPath $Dll)) { throw "Not found: $Dll (build it with Build-DicomIcons.ps1)" }
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($Dll))[0..7] | ForEach-Object { $_.ToString('x2') })
New-Item -ItemType Directory -Force $dir | Out-Null
$target = Join-Path $dir "DicomShell-$hash.dll"
if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $Dll -Destination $target }

Copy-Item -LiteralPath $schemaSource -Destination $schema -Force
$hr = [DicomPropsSetup.PropSys]::PSRegisterPropertySchema($schema)
if ($hr -lt 0) { throw ('PSRegisterPropertySchema failed: 0x{0:X8}' -f $hr) }
# INPLACE_S_TRUNCATED (0x000401A0): registered, but some part of the schema was not taken
if ($hr -ne 0) { Write-Host ('Schema registered with warnings: 0x{0:X8}' -f $hr) }

$k = $hklm.CreateSubKey("SOFTWARE\Classes\CLSID\$clsid")
$k.SetValue('', 'DicomStudyCount: properties of DICOM files')
# IInitializeWithFile (the path is needed for the duplicates): the indexer must not isolate it
$k.SetValue('DisableProcessIsolation', 1, 'DWord')
$k = $hklm.CreateSubKey("SOFTWARE\Classes\CLSID\$clsid\InprocServer32")
$k.SetValue('', $target)
$k.SetValue('ThreadingModel', 'Both')

$s = $hklm.CreateSubKey($state)
foreach ($ext in $extensions) {
    $h = $hklm.CreateSubKey("$handlers\$ext")
    $current = [string]$h.GetValue('')
    if ($current -and $current -ne $clsid) {
        Write-Host "$ext has a property handler already ($current): left as it is"
        continue
    }
    $h.SetValue('', $clsid)
    $s.SetValue("Owns$ext", 1, 'DWord')
}

# Copies of earlier builds no longer loaded anywhere
Get-ChildItem -LiteralPath $dir -Filter 'DicomShell-*.dll' | Where-Object { $_.FullName -ne $target } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
[DicomPropsSetup.PropSys]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)   # SHCNE_ASSOCCHANGED
Write-Host "DICOM properties registered: $target"
Write-Host 'Restart Explorer to see the new columns (right-click a column header > More...: "DICOM ...").'
