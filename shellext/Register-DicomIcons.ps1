<#
.SYNOPSIS
    Registers DicomIcons.dll for the current user (HKCU, no administrator rights): DICOM files without
    extension and .dcm files get the icon drawn by it. -Uninstall removes it. Called by
    Count-DicomStudies.ps1 -Install / -Uninstall.
.NOTES
    The DLL is copied to %LOCALAPPDATA%\DicomStudyCount\shellext under a name with its hash: Explorer
    keeps the registered one loaded, so a new build can never overwrite it.
    An extension that already belongs to another program is left alone (its icon is not changed).
#>
param(
    [string]$Dll,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults
if (-not $Dll) { $Dll = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'bin\DicomIcons.dll' }
$clsid = '{F4798986-2EEA-49F6-8082-2FC552CE691B}'
$classes = 'Software\Classes'
$state = 'Software\DicomStudyCount\IconHandler'      # what was registered, to undo only that
$types = @(
    @{ Ext = '.'; ProgId = 'DicomStudyCount.NoExtension'; Name = 'File' },
    @{ Ext = '.dcm'; ProgId = 'DicomStudyCount.Dcm'; Name = 'DICOM File' }
)
$hkcu = [Microsoft.Win32.Registry]::CurrentUser
Add-Type -Namespace DicomIconsSetup -Name Shell -MemberDefinition '
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);'

function Remove-Key([string]$path) { if ($hkcu.OpenSubKey($path)) { $hkcu.DeleteSubKeyTree($path) } }

if ($Uninstall) {
    $s = $hkcu.OpenSubKey($state)
    foreach ($t in $types) {
        # The extension is given back only if it still points to our type
        if ($s -and $s.GetValue("Owns$($t.Ext)") -eq 1) {
            $k = $hkcu.OpenSubKey("$classes\$($t.Ext)", $true)
            if ($k -and $k.GetValue('') -eq $t.ProgId) {
                $k.DeleteValue('', $false)
                if ($k.ValueCount -eq 0 -and $k.SubKeyCount -eq 0) { $k.Close(); $hkcu.DeleteSubKey("$classes\$($t.Ext)") }
            }
        }
        Remove-Key "$classes\$($t.ProgId)"
    }
    Remove-Key "$classes\CLSID\$clsid"
    Remove-Key 'Software\DicomStudyCount\IconHandler'
    [DicomIconsSetup.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)   # SHCNE_ASSOCCHANGED
    Write-Host 'DICOM file icons unregistered (Explorer frees the DLL when it restarts).'
    return
}

if (-not (Test-Path -LiteralPath $Dll)) { throw "Not found: $Dll (build it with Build-DicomIcons.ps1)" }
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($Dll))[0..7] | ForEach-Object { $_.ToString('x2') })
$dir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\shellext'
New-Item -ItemType Directory -Force $dir | Out-Null
$target = Join-Path $dir "DicomIcons-$hash.dll"
if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $Dll -Destination $target }

$k = $hkcu.CreateSubKey("$classes\CLSID\$clsid")
$k.SetValue('', 'DicomStudyCount: icons of DICOM files')
$k = $hkcu.CreateSubKey("$classes\CLSID\$clsid\InprocServer32")
$k.SetValue('', $target)
$k.SetValue('ThreadingModel', 'Apartment')

$s = $hkcu.CreateSubKey($state)
foreach ($t in $types) {
    $ext = $hkcu.CreateSubKey("$classes\$($t.Ext)")
    $current = [string]$ext.GetValue('')
    if ($current -and $current -ne $t.ProgId) {
        Write-Host "$($t.Ext) belongs to $current already: its icons are left as they are"
        continue
    }
    $p = $hkcu.CreateSubKey("$classes\$($t.ProgId)")
    $p.SetValue('', $t.Name)
    # "%1": the icon of each file comes from its icon handler
    $hkcu.CreateSubKey("$classes\$($t.ProgId)\DefaultIcon").SetValue('', '%1')
    $hkcu.CreateSubKey("$classes\$($t.ProgId)\ShellEx\IconHandler").SetValue('', $clsid)
    $ext.SetValue('', $t.ProgId)
    $s.SetValue("Owns$($t.Ext)", 1, 'DWord')
}
[DicomIconsSetup.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)   # SHCNE_ASSOCCHANGED

# Copies of earlier builds that Explorer no longer has loaded
Get-ChildItem -LiteralPath $dir -Filter 'DicomIcons-*.dll' | Where-Object { $_.FullName -ne $target } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
Write-Host "DICOM file icons registered: $target"
