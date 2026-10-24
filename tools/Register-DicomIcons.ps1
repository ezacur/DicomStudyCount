<#
.SYNOPSIS
    Registers, for the current user (HKCU, no administrator rights), what bin\DicomShell.dll does for
    DICOM files without extension, .dcm, .ima and .dicom: their icon, their image and header in
    Explorer's preview pane, their thumbnail, the DICOM values shown when hovering them and in the
    Details pane, and "Show DICOM duplicates" in their context menu. -Uninstall removes it all.
    Called by Count-DicomStudies.ps1 -Install / -Uninstall.
.NOTES
    The DLL is copied to %LOCALAPPDATA%\DicomStudyCount\shellext under a name with its hash: Explorer
    keeps the registered one loaded, so the one in bin\ can always be replaced.
    An extension that already belongs to another program keeps its icon (the icon belongs to the
    program that opens it); the rest goes into SystemFileAssociations, which Windows reads for every
    file of that extension whoever owns it.
#>
param(
    [string]$Dll,
    [string]$Script,        # Count-DicomStudies.ps1, for the "Show DICOM duplicates" verb
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no $PSScriptRoot in parameter defaults
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Dll) { $Dll = Join-Path $root 'bin\DicomShell.dll' }
if (-not $Script) { $Script = Join-Path $root 'Count-DicomStudies.ps1' }
$clsid = '{F4798986-2EEA-49F6-8082-2FC552CE691B}'           # icons
$previewClsid = '{F3E184D5-95DF-4BE9-9929-32FF8A59CD55}'    # preview (run by Windows in prevhost.exe)
$thumbClsid = '{F6D7D002-C00D-4893-AC73-DC78D44316C3}'      # thumbnails
$previewShellEx = 'ShellEx\{8895b1c6-b41f-4c1c-a562-0d564250836f}'   # IPreviewHandler
$thumbShellEx = 'ShellEx\{e357fccd-a995-4576-b01f-234630154e96}'     # IThumbnailProvider
$prevhostAppId = '{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}'
$previewList = 'Software\Microsoft\Windows\CurrentVersion\PreviewHandlers'
$classes = 'Software\Classes'
$sfa = 'Software\Classes\SystemFileAssociations'
$state = 'Software\DicomStudyCount\IconHandler'      # what was registered, to undo only that
$previewSettings = 'Software\DicomStudyCount\Preview'  # the split, and the tab of each folder
$types = @(
    @{ Ext = '.'; ProgId = 'DicomStudyCount.NoExtension'; Name = 'File' },
    @{ Ext = '.dcm'; ProgId = 'DicomStudyCount.Dcm'; Name = 'DICOM File' },
    @{ Ext = '.ima'; ProgId = 'DicomStudyCount.Ima'; Name = 'DICOM File (IMA)' },
    @{ Ext = '.dicom'; ProgId = 'DicomStudyCount.Dicom'; Name = 'DICOM File' }
)
$verbKey = 'shell\DicomStudyCountDuplicates'

# DICOM values shown when hovering a file, in the Details pane and in Properties > Details
$d = 'DicomStudyCount.'
$infoTip = 'prop:' + (('Modality', 'SeriesNumber', 'SeriesDescription', 'InstanceNumber', 'Plane', 'ZLevel', 'Acquired',
    'DuplicatedIn' | ForEach-Object { $d + $_ }) -join ';')
$previewDetails = 'prop:' + ((('StudyHash', 'SeriesHash', 'Modality', 'SeriesNumber', 'SeriesDescription', 'InstanceNumber', 'Rows',
    'Columns', 'Plane', 'ZLevel', 'TriggerTime', 'Acquired', 'ObjectType', 'Compression', 'DuplicatedIn' | ForEach-Object { $d + $_ }) +
    'System.Size', 'System.DateModified') -join ';')
$fullDetails = 'prop:System.PropGroup.Description;' + (('StudyHash', 'SeriesHash', 'Modality', 'SeriesNumber', 'InstanceNumber',
    'SeriesDescription', 'Protocol', 'Plane', 'Acquired', 'TriggerTime', 'ZLevel', 'Rows', 'Columns', 'PixelSpacing', 'SliceThickness',
    'Frames', 'ImageType', 'ObjectType', 'Compression', 'Manufacturer', 'FrameOfReference', 'StudyDate', 'PatientID', 'SensitiveData',
    'ImageOrientation', 'StudyInstanceUID', 'SeriesInstanceUID', 'DuplicatedIn' | ForEach-Object { $d + $_ }) -join ';') +
    ';System.PropGroup.FileSystem;System.ItemNameDisplay;System.ItemTypeText;System.ItemFolderPathDisplay;System.Size;' +
    'System.DateCreated;System.DateModified;System.FileAttributes'

$hkcu = [Microsoft.Win32.Registry]::CurrentUser
Add-Type -Namespace DicomIconsSetup -Name Shell -MemberDefinition '
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);'

function Remove-Key([string]$path) { if ($hkcu.OpenSubKey($path)) { $hkcu.DeleteSubKeyTree($path) } }

# What goes into a file type: the icon (only in a ProgID of ours), and the rest (preview, thumbnails,
# values, verb), each in one place only, or Explorer would list the verb twice: in
# SystemFileAssociations\<ext> for an extension, in the ProgID for files without extension
function Set-FileType([string]$key, [bool]$icon, [bool]$rest) {
    if ($icon) {
        # "%1": the icon of each file comes from its icon handler
        $hkcu.CreateSubKey("$key\DefaultIcon").SetValue('', '%1')
        $hkcu.CreateSubKey("$key\ShellEx\IconHandler").SetValue('', $clsid)
        # No icon of the program over the thumbnails (the console of the verb, on the series badge)
        $hkcu.CreateSubKey($key).SetValue('TypeOverlay', '')
    }
    if (-not $rest) { return }
    $hkcu.CreateSubKey("$key\$previewShellEx").SetValue('', $previewClsid)
    $hkcu.CreateSubKey("$key\$thumbShellEx").SetValue('', $thumbClsid)
    $k = $hkcu.CreateSubKey($key)
    $k.SetValue('InfoTip', $infoTip)
    $k.SetValue('PreviewDetails', $previewDetails)
    $k.SetValue('FullDetails', $fullDetails)
    $v = $hkcu.CreateSubKey("$key\$verbKey")
    $v.SetValue('MUIVerb', 'Show DICOM duplicates')
    $v.SetValue('Icon', 'imageres.dll,-68')
    # conhost --headless runs PowerShell without opening a console window
    $hkcu.CreateSubKey("$key\$verbKey\command").SetValue('',
        ('conhost.exe --headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "%1" -ShowDuplicates' -f $Script))
}

# Undo Set-FileType in SystemFileAssociations\<ext>, leaving whatever others put there
function Clear-Sfa([string]$key) {
    $k = $hkcu.OpenSubKey($key, $true)
    if (-not $k) { return }
    foreach ($n in 'InfoTip', 'PreviewDetails', 'FullDetails') { if ($k.GetValue($n) -like '*DicomStudyCount.*') { $k.DeleteValue($n, $false) } }
    foreach ($sub in $previewShellEx, $thumbShellEx) {
        $s = $hkcu.OpenSubKey("$key\$sub")
        if ($s -and ($s.GetValue('') -eq $previewClsid -or $s.GetValue('') -eq $thumbClsid)) { $s.Close(); $hkcu.DeleteSubKeyTree("$key\$sub") }
    }
    Remove-Key "$key\$verbKey"
    foreach ($parent in 'ShellEx', 'shell') { $p = $hkcu.OpenSubKey("$key\$parent"); if ($p -and $p.SubKeyCount -eq 0 -and $p.ValueCount -eq 0) { $p.Close(); $hkcu.DeleteSubKey("$key\$parent") } }
    $k = $hkcu.OpenSubKey($key)
    if ($k -and $k.SubKeyCount -eq 0 -and $k.ValueCount -eq 0) { $k.Close(); $hkcu.DeleteSubKey($key) }
}

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
        if ($t.Ext -ne '.') { Clear-Sfa "$sfa\$($t.Ext)" }
    }
    foreach ($c in $clsid, $previewClsid, $thumbClsid) { Remove-Key "$classes\CLSID\$c" }
    $list = $hkcu.OpenSubKey($previewList, $true)
    if ($list) { $list.DeleteValue($previewClsid, $false) }
    Remove-Key $state
    Remove-Key $previewSettings
    [DicomIconsSetup.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)   # SHCNE_ASSOCCHANGED
    Write-Host 'DICOM file icons, preview and thumbnails unregistered (Explorer frees the DLL when it restarts).'
    return
}

if (-not (Test-Path -LiteralPath $Dll)) { throw "Not found: $Dll (build it with tools\Build.ps1)" }
$sha = [Security.Cryptography.SHA256]::Create()
$hash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($Dll))[0..7] | ForEach-Object { $_.ToString('x2') })
$dir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\shellext'
New-Item -ItemType Directory -Force $dir | Out-Null
$target = Join-Path $dir "DicomShell-$hash.dll"
if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $Dll -Destination $target }

foreach ($c in @(@($clsid, 'DicomStudyCount: icons of DICOM files'), @($previewClsid, 'DicomStudyCount: preview of DICOM files'),
                 @($thumbClsid, 'DicomStudyCount: thumbnails of DICOM files'))) {
    $k = $hkcu.CreateSubKey("$classes\CLSID\$($c[0])")
    $k.SetValue('', $c[1])
    # (Its DisableLowILProcessIsolation goes to HKLM, where Explorer reads it: Register-DicomProperties.ps1)
    if ($c[0] -eq $previewClsid) { $k.SetValue('DisplayName', 'DICOM preview'); $k.SetValue('AppID', $prevhostAppId) }
    $k = $hkcu.CreateSubKey("$classes\CLSID\$($c[0])\InprocServer32")
    $k.SetValue('', $target)
    $k.SetValue('ThreadingModel', 'Apartment')
}
$hkcu.CreateSubKey($previewList).SetValue($previewClsid, 'DicomStudyCount: preview of DICOM files')
# Tags or names, of the preview before its tabs
$p = $hkcu.OpenSubKey($previewSettings, $true)
if ($p) { $p.DeleteValue('HeaderNames', $false) }

$s = $hkcu.CreateSubKey($state)
foreach ($t in $types) {
    $ext = $hkcu.CreateSubKey("$classes\$($t.Ext)")
    $current = [string]$ext.GetValue('')
    if ($current -and $current -ne $t.ProgId) {
        Write-Host "$($t.Ext) belongs to $current already: it keeps its icon"
    }
    else {
        $hkcu.CreateSubKey("$classes\$($t.ProgId)").SetValue('', $t.Name)
        Set-FileType "$classes\$($t.ProgId)" $true ($t.Ext -eq '.')
        $ext.SetValue('', $t.ProgId)
        $s.SetValue("Owns$($t.Ext)", 1, 'DWord')
    }
    # Whoever owns it (now or later): preview, thumbnails, values and the verb
    if ($t.Ext -ne '.') { Set-FileType "$sfa\$($t.Ext)" $false $true }
}
[DicomIconsSetup.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)   # SHCNE_ASSOCCHANGED

# Copies of earlier builds that Explorer no longer has loaded (DicomIcons-*: their older name)
Get-ChildItem -LiteralPath $dir -Filter 'Dicom*-*.dll' | Where-Object { $_.FullName -ne $target } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
Write-Host "DICOM file icons, preview and thumbnails registered: $target"
