<#
.SYNOPSIS
    Registers the DICOM property handler of bin\DicomShell.dll for the whole machine (it needs administrator
    rights: Windows reads property handlers only from HKLM). -Uninstall removes it.
.NOTES
    The DLL is copied to %ProgramFiles%\explorer4dicoms, which only administrators can change: Explorer
    and the search indexer load it. Its name carries its hash, since a loaded DLL cannot be overwritten.
    An extension that already has a property handler of another program is chained: this one goes in
    front of it, and gives every file that is not DICOM to it (recorded in Chain\<ext> of the state key
    below, read by the DLL); -Uninstall puts it back. -Img adds .img (remembered).
#>
param(
    [string]$Dll,
    [switch]$Img,
    [switch]$Uninstall,
    # The folder of the data of the suite of the user who installs it (its copy of the DLL)
    [string]$UserData = (Join-Path $env:LOCALAPPDATA 'explorer4dicoms')
)
$ErrorActionPreference = 'Stop'
if (-not (New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator rights are needed (run it elevated)'
}
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults
$bin = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'bin'
if (-not $Dll) { $Dll = Join-Path $bin 'DicomShell.dll' }

$clsid = '{21329A2B-09B3-4250-9103-9267877321EB}'
$previewClsid = '{F3E184D5-95DF-4BE9-9929-32FF8A59CD55}'     # the preview, see below
$handlers = 'SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers'
$state = 'SOFTWARE\explorer4dicoms\PropertyHandler'      # what was registered, to undo only that
$extensions = '.', '.dcm', '.ima', '.dicom'                 # '.': files without extension
$hklm = [Microsoft.Win32.Registry]::LocalMachine
$st = $hklm.OpenSubKey($state)
if ($Img -or $Uninstall -or ($st -and $st.GetValue('Img') -eq 1)) { $extensions += '.img' }
function Get-ChainName([string]$ext) { if ($ext -eq '.') { '(none)' } else { $ext } }
$dir = Join-Path $env:ProgramFiles 'explorer4dicoms'
# The columns (names, types) of the properties: a schema registered from its installed copy
$schemaSource = Join-Path $bin 'explorer4dicoms.propdesc'
$schema = Join-Path $dir 'explorer4dicoms.propdesc'
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
        if ($s -and $s.GetValue("Owns$ext") -eq 1 -and $k -and $k.GetValue('') -eq $clsid) {
            # The handler from before, if there was one
            $before = $hklm.OpenSubKey("$state\Chain\$(Get-ChainName $ext)")
            $previous = if ($before) { [string]$before.GetValue('Properties') }
            if ($previous) { $hklm.CreateSubKey("$handlers\$ext").SetValue('', $previous) } else { Remove-Key "$handlers\$ext" }
        }
    }
    Remove-Key "SOFTWARE\Classes\CLSID\$clsid"
    Remove-Key "SOFTWARE\Classes\CLSID\$previewClsid"
    Remove-Key $state
    # The copies of the DLL that nothing has loaded (the rest go on a later -Uninstall), and the folder
    # and the key of the suite once empty
    Get-ChildItem -LiteralPath $dir -Filter 'DicomShell-*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
    if ((Test-Path -LiteralPath $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir -Force }
    $suite = $hklm.OpenSubKey('SOFTWARE\explorer4dicoms')
    if ($suite -and -not $suite.SubKeyCount -and -not $suite.ValueCount) { $suite.Close(); $hklm.DeleteSubKey('SOFTWARE\explorer4dicoms') }
    Write-Host 'DICOM properties unregistered (Explorer frees the DLL when it restarts).'
    return
}

if (-not (Test-Path -LiteralPath $Dll)) { throw "Not found: $Dll (build it with tools\Build.ps1)" }
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($Dll))[0..7] | ForEach-Object { $_.ToString('x2') })
New-Item -ItemType Directory -Force $dir | Out-Null
$target = Join-Path $dir "DicomShell-$hash.dll"
if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $Dll -Destination $target }
# A -FullClean before may have left the copies of the DLL (in use then) to be deleted at the next
# restart: installed again, they stay
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'PendingDeletes.ps1')
$kept = Remove-PendingDeletes -Under $dir, $UserData
if ($kept) { Write-Host "No longer deleted at the next restart: $kept files and folders of a previous full clean" }

Copy-Item -LiteralPath $schemaSource -Destination $schema -Force
$hr = [DicomPropsSetup.PropSys]::PSRegisterPropertySchema($schema)
if ($hr -lt 0) { throw ('PSRegisterPropertySchema failed: 0x{0:X8}' -f $hr) }
# INPLACE_S_TRUNCATED (0x000401A0): registered, but some part of the schema was not taken
if ($hr -ne 0) { Write-Host ('Schema registered with warnings: 0x{0:X8}' -f $hr) }

$k = $hklm.CreateSubKey("SOFTWARE\Classes\CLSID\$clsid")
$k.SetValue('', 'explorer4dicoms: properties of DICOM files')
# IInitializeWithFile (the path is needed for the duplicates): the indexer must not isolate it
$k.SetValue('DisableProcessIsolation', 1, 'DWord')
$k = $hklm.CreateSubKey("SOFTWARE\Classes\CLSID\$clsid\InprocServer32")
$k.SetValue('', $target)
$k.SetValue('ThreadingModel', 'Both')

# The preview (registered per user by Register-DicomIcons.ps1) at medium integrity, not the low one of
# prevhost.exe: its tab "Columns" changes the columns of the Explorer window, which a low-integrity
# process cannot reach. Explorer takes this value only from HKLM, never from the user's classes.
$k = $hklm.CreateSubKey("SOFTWARE\Classes\CLSID\$previewClsid")
$k.SetValue('DisableLowILProcessIsolation', 1, 'DWord')

$s = $hklm.CreateSubKey($state)
if ($extensions -contains '.img') { $s.SetValue('Img', 1, 'DWord') }
foreach ($ext in $extensions) {
    $h = $hklm.CreateSubKey("$handlers\$ext")
    $current = [string]$h.GetValue('')
    if ($current -and $current -ne $clsid) {
        $s.CreateSubKey("Chain\$(Get-ChainName $ext)").SetValue('Properties', $current)
        Write-Host "$ext has a property handler already ($current): chained (files that are not DICOM keep it)"
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
