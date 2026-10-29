<#
.SYNOPSIS
    explorer4dicoms: Windows Explorer for a bag of DICOM files. Counts the distinct DICOM studies
    (StudyInstanceUID) in a folder and all its subfolders, and installs the rest of the suite.

.DESCRIPTION
    Reads only the header of each file (it stops at InstanceNumber, tag 0020,0013), so pixel
    data is never loaded. Supports Explicit/Implicit VR Little Endian, Explicit VR Big Endian,
    Deflated and "raw" DICOM files without preamble. Non-DICOM files are ignored.

.EXAMPLE
    .\explorer4dicoms.ps1 D:\Patients
.EXAMPLE
    .\explorer4dicoms.ps1 D:\Patients -Detail
.EXAMPLE
    .\explorer4dicoms.ps1 D:\Patients -Csv C:\temp\studies.csv
.EXAMPLE
    .\explorer4dicoms.ps1 D:\Patients -WriteComments    # fills Explorer's Comments column
    .\explorer4dicoms.ps1 D:\Patients -Clean            # as if it had never been counted
.EXAMPLE
    .\explorer4dicoms.ps1 D:\Patients -ShowTree         # folder tree with the comments
.EXAMPLE
    .\explorer4dicoms.ps1 -RestoreQuarantine D:\_DicomQuarantine\Patients\20261022_091500
.EXAMPLE
    .\explorer4dicoms.ps1 -Install      # context menu + background watcher (-Watch) at logon
    .\explorer4dicoms.ps1 -Install -Img # also DICOM files named .img (remembered)
    .\explorer4dicoms.ps1 -Uninstall
    .\explorer4dicoms.ps1 -FullClean    # as before the suite: every count cleaned, uninstalled, its data deleted
.NOTES
    From a zip: Install.cmd and Uninstall.cmd (-FullClean) of this folder do it with a double click.
#>
[CmdletBinding(DefaultParameterSetName = 'Scan')]
param(
    [Parameter(ParameterSetName = 'Scan', Mandatory = $true, Position = 0)]
    [string]$Path,

    # Shows a table with every study (date, modality, patient, description, series, files)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$Detail,

    # Exports the study list to CSV (';' separator so Excel opens it directly in Spanish locales)
    [Parameter(ParameterSetName = 'Scan')]
    [string]$Csv,

    # Writes the study summary into the Comments column of every folder (via desktop.ini)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$WriteComments,

    # Leaves Explorer as if the folder and its subfolders had never been counted: removes the comments
    # and icons, the Comments column (open windows and saved views) and their data in the index and
    # the problems reports; stops a count running on them
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$Clean,

    # Shows the DICOM columns of the files in the Explorer windows open on the folder (or on its
    # parent folder), or hides them if any is visible
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$ToggleColumns,

    # Removes the "downloaded from the Internet" mark from the DICOM files of the folder and its
    # subfolders, so that Windows shows them in the preview pane (their contents are not touched)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$Unblock,

    # -Path is a DICOM file: opens the folders of its copies found by the counts, with them selected
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$ShowDuplicates,

    # Opens a window with the folder tree and the comment of every folder (reads desktop.ini only)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$ShowTree,

    # Waits for a key press when done
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$Pause,

    # Background process: adds the Comments column when browsing folders with DICOM comments
    [Parameter(ParameterSetName = 'Watch', Mandatory = $true)]
    [switch]$Watch,

    # Moves back the files of a quarantine batch folder (the one holding quarantine-log.csv)
    [Parameter(ParameterSetName = 'Restore', Mandatory = $true)]
    [string]$RestoreQuarantine,

    [Parameter(ParameterSetName = 'Install', Mandatory = $true)]
    [switch]$Install,
    [Parameter(ParameterSetName = 'Install')]
    [switch]$Img,

    [Parameter(ParameterSetName = 'Uninstall', Mandatory = $true)]
    [switch]$Uninstall,

    # Leaves the computer as before the suite: -Clean on every counted folder, -Uninstall, and its data,
    # caches and settings deleted (never the DICOM files, nor the quarantines)
    [Parameter(ParameterSetName = 'FullClean', Mandatory = $true)]
    [switch]$FullClean
)

$ErrorActionPreference = 'Stop'

# The name of the suite: where its data and settings are, and what it writes in the folders and the registry
$appName = 'explorer4dicoms'
$appData = Join-Path $env:LOCALAPPDATA $appName

# Context menu: a "DICOM" submenu with fixed entries (the menu cannot depend on whether the folder
# was counted: Explorer does not read desktop.ini when it builds it). Key names set the order.
$menuName = 'explorer4dicoms'
$menuEntries = @(
    @{ Name = '1Count'; Label = 'Count studies'; Args = '-WriteComments' },
    @{ Name = '2Tree'; Label = 'Show tree'; Args = '-ShowTree' },
    @{ Name = '3Clean'; Label = 'Clean'; Args = '-Clean' },
    @{ Name = '4Columns'; Label = 'Show/hide DICOM columns'; Args = '-ToggleColumns' },
    @{ Name = '5Unblock'; Label = 'Unblock (allow preview)'; Args = '-Unblock' },
    # (not the script: the explorer of bin\DicomShell.dll, the copy the icons use)
    @{ Name = '6Explorer'; Label = 'DICOM explorer'; Explorer = '' },
    # (the same, writing the report of the folder in %TEMP%\explorer4dicoms\reports and opening it)
    @{ Name = '7Report'; Label = 'DICOM report'; Explorer = '/report ' }
)
# The DICOM columns of the files (bin\explorer4dicoms.propdesc), in the order they are shown
$dicomColumns = 'StudyHash', 'SeriesHash', 'Modality', 'SeriesNumber', 'InstanceNumber', 'SeriesDescription',
    'Plane', 'SeriesKind', 'Segmentation', 'Acquired', 'TriggerTime', 'ZLevel', 'Rows', 'Columns', 'PixelSpacing', 'SliceThickness', 'Frames',
    'ImageType', 'ObjectType', 'Compression', 'Protocol', 'Manufacturer', 'FrameOfReference', 'StudyDate',
    'PatientID', 'SensitiveData', 'ImageOrientation', 'StudyInstanceUID', 'SeriesInstanceUID', 'DuplicatedIn' |
    ForEach-Object { "Explorer4Dicoms.$_" }
$menuParents = @(
    'HKCU:\Software\Classes\Directory\shell',
    'HKCU:\Software\Classes\Directory\Background\shell'
)
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'explorer4dicomsWatcher'
# conhost --headless runs PowerShell without opening a console window
$watchCommand = 'conhost.exe --headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Watch' -f $PSCommandPath

# Runs a script with administrator rights (UAC prompt unless already elevated) and shows its output
function Invoke-Elevated([string]$script, [string[]]$arguments) {
    # Parameter names as they are, values quoted
    $quoted = ($arguments | ForEach-Object { if ($_ -match '^-\w+$') { $_ } else { "'" + ($_ -replace "'", "''") + "'" } }) -join ' '
    $principal = New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Invoke-Expression "& '$script' $quoted"
        return
    }
    $log = [IO.Path]::GetTempFileName()
    $command = "try { & '$script' $quoted *>&1 | Out-File -LiteralPath '$log' } catch { `$_ | Out-String | Add-Content -LiteralPath '$log'; exit 1 }"
    try {
        $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
            -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command
        Get-Content -LiteralPath $log | ForEach-Object { Write-Host $_ }
        if ($p.ExitCode -ne 0) { Write-Host "Failed: $script" -ForegroundColor Yellow }
    }
    catch { Write-Host "Not done (administrator rights refused): $script" -ForegroundColor Yellow }
    finally { Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue }
}

function Stop-Watcher {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.CommandLine -match 'explorer4dicoms\.ps1.*-Watch' -and $_.ProcessId -ne $PID } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Install) {
    Set-ItemProperty -LiteralPath $runKey -Name $runName -Value $watchCommand
    Stop-Watcher
    # (not from this folder: it would hold it, and the folder could not be renamed or moved)
    Start-Process conhost.exe -ArgumentList ($watchCommand -replace '^conhost\.exe ', '') -WorkingDirectory $env:USERPROFILE
    # The copy of bin\DicomShell.dll that tools\Register-DicomIcons.ps1 registers (by its hash)
    $shellDll = Join-Path $PSScriptRoot 'bin\DicomShell.dll'
    $sha = [Security.Cryptography.SHA256]::Create()
    $dllHash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($shellDll))[0..7] | ForEach-Object { $_.ToString('x2') })
    $shellCopy = Join-Path $appData "shellext\DicomShell-$dllHash.dll"
    foreach ($parent in $menuParents) {
        $menu = "$parent\$menuName"
        if (Test-Path -LiteralPath $menu) { Remove-Item -LiteralPath $menu -Recurse -Force }
        New-Item -Path $menu -Force | Out-Null
        Set-ItemProperty -LiteralPath $menu -Name 'MUIVerb' -Value 'DICOM'
        Set-ItemProperty -LiteralPath $menu -Name 'Icon' -Value 'imageres.dll,-68'
        # An empty SubCommands makes it a submenu with the verbs of its "shell" subkey
        Set-ItemProperty -LiteralPath $menu -Name 'SubCommands' -Value ''
        foreach ($entry in $menuEntries) {
            $key = "$menu\shell\$($entry.Name)"
            # conhost --headless runs PowerShell without opening a console window
            $command = 'conhost.exe --headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "%V" {1}' -f $PSCommandPath, $entry.Args
            if ($null -ne $entry.Explorer) { $command = 'rundll32.exe "{0}",Explore {1}"%V"' -f $shellCopy, $entry.Explorer }
            New-Item -Path "$key\command" -Force | Out-Null
            Set-ItemProperty -LiteralPath $key -Name 'MUIVerb' -Value $entry.Label
            Set-ItemProperty -LiteralPath "$key\command" -Name '(default)' -Value $command
        }
    }
    # Icons of the DICOM files (bin\DicomShell.dll, registered for the user)
    & (Join-Path $PSScriptRoot 'tools\Register-DicomIcons.ps1') -Dll $shellDll -Script $PSCommandPath -Img:$Img
    # The columns of the DICOM files: Windows takes property handlers from HKLM only. No UAC prompt
    # when the DLL and the column schema installed are already these ones.
    $server = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Classes\CLSID\{21329A2B-09B3-4250-9103-9267877321EB}\InprocServer32')
    $installedSchema = Join-Path $env:ProgramFiles 'explorer4dicoms\explorer4dicoms.propdesc'
    $handlers = 'SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers'
    # (and the preview at medium integrity, also from HKLM)
    $preview = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Classes\CLSID\{F3E184D5-95DF-4BE9-9929-32FF8A59CD55}')
    # Every extension ours (or chained to ours: Register-DicomProperties.ps1)
    $extensions = '.', '.dcm', '.ima', '.dicom'
    $propState = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\explorer4dicoms\PropertyHandler')
    if ($Img -or ($propState -and $propState.GetValue('Img') -eq 1)) { $extensions += '.img' }
    $upToDate = $server -and [IO.Path]::GetFileName([string]$server.GetValue('')) -eq "DicomShell-$dllHash.dll" -and
        $preview -and $preview.GetValue('DisableLowILProcessIsolation') -eq 1 -and
        @($extensions | Where-Object {
            # (through .NET: a key named "." is the current one for the registry provider of PowerShell)
            $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$handlers\$_")
            -not $k -or [string]$k.GetValue('') -ne '{21329A2B-09B3-4250-9103-9267877321EB}' }).Count -eq 0 -and
        (Test-Path -LiteralPath $installedSchema) -and
        ((Get-FileHash -LiteralPath $installedSchema).Hash -eq (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'bin\explorer4dicoms.propdesc')).Hash)
    # (a -FullClean before may have left the copies of the DLL to be deleted at the next restart: the
    # elevated part takes that back)
    . (Join-Path $PSScriptRoot 'tools\PendingDeletes.ps1')
    $upToDate = $upToDate -and -not @(Get-PendingDeletes -Under (Join-Path $env:ProgramFiles $appName), $appData).Count
    if ($upToDate) { Write-Host 'DICOM columns already installed and up to date' }
    else { Invoke-Elevated (Join-Path $PSScriptRoot 'tools\Register-DicomProperties.ps1') (@('-Dll', $shellDll) + @(if ($Img) { '-Img' })) }
    Write-Host 'Installed. Right-click a folder: DICOM >'
    $menuEntries | ForEach-Object { Write-Host "  - $($_.Label)" }
    Write-Host '(on Windows 11 it is under "Show more options" when using the new context menu).'
    Write-Host 'Background watcher running and registered to start at logon.'
    return
}

function Invoke-Uninstall([switch]$Purge) {
    Remove-ItemProperty -LiteralPath $runKey -Name $runName -ErrorAction SilentlyContinue
    Stop-Watcher
    foreach ($parent in $menuParents) {
        if (Test-Path -LiteralPath "$parent\$menuName") { Remove-Item -LiteralPath "$parent\$menuName" -Recurse -Force }
    }
    & (Join-Path $PSScriptRoot 'tools\Register-DicomIcons.ps1') -Uninstall
    Remove-Item -LiteralPath "HKCU:\Software\$appName\Explorer" -Recurse -Force -ErrorAction SilentlyContinue
    # The reports of the explorer, in a temporary folder of their own
    Remove-Item -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "$appName\reports") -Recurse -Force -ErrorAction SilentlyContinue
    # (with -Purge also the copies of the DLL in %ProgramFiles%, when they are all that is left there)
    if ([Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SOFTWARE\$appName\PropertyHandler") -or
        ($Purge -and ([Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SOFTWARE\$appName") -or
                      (Test-Path -LiteralPath (Join-Path $env:ProgramFiles $appName))))) {
        Invoke-Elevated (Join-Path $PSScriptRoot 'tools\Register-DicomProperties.ps1') @('-Uninstall')
    }
    Write-Host 'Uninstalled.'
}

if ($Uninstall) {
    Invoke-Uninstall
    return
}

function Get-DesktopIniFiles([string]$root) {
    Get-ChildItem -LiteralPath $root -Recurse -Force -Filter 'desktop.ini' -ErrorAction SilentlyContinue |
        Where-Object { -not $_.PSIsContainer }
}

# Removes the Comments column from the Explorer windows open on the folder or any of its subfolders
# (and on its parent folder with -Parent)
function Hide-CommentColumn([string]$root, [switch]$Parent) {
    $parentPath = Split-Path -Path $root -Parent
    $windows = (New-Object -ComObject Shell.Application).Windows()
    for ($i = 0; $i -lt $windows.Count; $i++) {
        try {
            $window = $windows.Item($i)
            $windowPath = $window.Document.Folder.Self.Path
            if ([Explorer4Dicoms.Paths]::IsUnder($windowPath, $root) -or ($Parent -and $windowPath -eq $parentPath)) {
                [void][Explorer4Dicoms.ExplorerColumns]::HideCommentColumn($window)
            }
        }
        catch { }   # windows that are not file system folders (This PC, Home...)
    }
}

# Stops the counts running on the folder, inside it, or on a folder that contains it: they would
# write their comments again over the cleaned folders
function Stop-CountsOn([string]$root) {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.CommandLine -match 'explorer4dicoms\.ps1.*-WriteComments' -and $_.ProcessId -ne $PID } |
        ForEach-Object {
            # Counts are started with -Path "<folder>" -WriteComments (conhost --headless drops the
            # quotes, so the folder is whatever lies between both parameters)
            if ($_.CommandLine -notmatch '-Path\s+(.+?)\s+-WriteComments\b') { return }
            $p = $Matches[1].Trim().Trim('"').TrimEnd('\')
            if ($p -match '^[A-Za-z]:$') { $p += '\' }
            if ([Explorer4Dicoms.Paths]::IsUnder($p, $root) -or [Explorer4Dicoms.Paths]::IsUnder($root, $p)) {
                Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
                Write-Host "Stopped the count running on $p"
            }
        }
}

# Shows or hides the DICOM columns in the Explorer windows open on the folder (menu on the background
# of a window) or on its parent folder (menu on a folder of the list): hidden if any is visible in the
# first of those windows, shown otherwise, and the same in all of them
function Switch-DicomColumns([string]$root) {
    $parent = Split-Path -Path $root -Parent
    $targets = @()
    $windows = (New-Object -ComObject Shell.Application).Windows()
    for ($i = 0; $i -lt $windows.Count; $i++) {
        try {
            $window = $windows.Item($i)
            $windowPath = $window.Document.Folder.Self.Path
            if ($windowPath -eq $root -or ($parent -and $windowPath -eq $parent)) { $targets += $window }
        }
        catch { }   # windows that are not file system folders (This PC, Home...)
    }
    if (-not $targets) { Write-Host "No Explorer window open on $root"; return }
    $show = -not [Explorer4Dicoms.ExplorerColumns]::AnyColumnShown($targets[0], [string[]]$dicomColumns)
    foreach ($w in $targets) {
        try { [Explorer4Dicoms.ExplorerColumns]::SetColumnsShown($w, [string[]]$dicomColumns, $show) } catch { }
    }
    Write-Host ('DICOM columns {0} in {1} window(s)' -f $(if ($show) { 'shown' } else { 'hidden' }), $targets.Count)
}

# Adds the Comments column (and fits it to the text) in the Explorer windows open on the
# folder, on its parent folder or on any of its subfolders
function Show-CommentColumn([string]$root) {
    $parent = Split-Path -Path $root -Parent
    # ShellWindows does not enumerate reliably with foreach in PowerShell: iterate by index
    $windows = (New-Object -ComObject Shell.Application).Windows()
    for ($i = 0; $i -lt $windows.Count; $i++) {
        try {
            $window = $windows.Item($i)
            $windowPath = $window.Document.Folder.Self.Path
            if ($windowPath -eq $root -or $windowPath -eq $parent -or $windowPath.StartsWith($root.TrimEnd('\') + '\', 'OrdinalIgnoreCase')) {
                [void][Explorer4Dicoms.ExplorerColumns]::ShowCommentColumn($window)
            }
        }
        catch { }   # windows that are not file system folders (This PC, Home...)
    }
}

# The C# part (DICOM reader, count, folder comments and icons, Explorer columns): bin\Explorer4Dicoms.dll,
# built from src\Explorer4Dicoms\*.cs by tools\Build.ps1. Loaded from its bytes, so the file is never
# locked: the folder can be updated while the background watcher runs.
if (-not ('Explorer4Dicoms.Scanner' -as [type])) {
    $assembly = Join-Path $PSScriptRoot 'bin\Explorer4Dicoms.dll'
    if (-not (Test-Path -LiteralPath $assembly)) { throw "Not found: $assembly (build it with tools\Build.ps1)" }
    Add-Type -AssemblyName System.Drawing
    [void][Reflection.Assembly]::Load([IO.File]::ReadAllBytes($assembly))
}

# Problems reports (split studies, duplicated files) live outside the scanned folders,
# one JSON file per scanned root
$reportDir = Join-Path $appData 'reports'
# Index of counted locations, to find copies across them (one file per counted folder)
$indexDir = Join-Path $appData 'index'
# Copies of every file found by the counts, for the "DICOM duplicated in" column (written by the
# counts next to the index, read by the property handler of bin\DicomShell.dll)
$copiesDir = Join-Path $appData 'duplicates'

function Get-ReportPath([string]$root) {
    $sha = [Security.Cryptography.SHA1]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant())) | ForEach-Object { $_.ToString('x2') })
    Join-Path $reportDir "$hash.json"
}

function Save-ProblemsReport([string]$root, $result) {
    New-Item -ItemType Directory -Force $reportDir | Out-Null
    $report = [ordered]@{
        Root            = $root
        ScannedAt       = (Get-Date).ToString('s')
        DuplicatedFiles = $result.DuplicatedFiles
        Splits          = @($result.Splits | ForEach-Object { [ordered]@{ Tag = $_.Tag; StudyInstanceUID = $_.StudyInstanceUID; Folders = @($_.Folders) } })
        Duplicates      = @($result.Duplicates | ForEach-Object {
            [ordered]@{
                Files     = $_.Files
                Identical = $_.Identical
                Different = $_.Different
                Keep      = $_.Keep
                Folders   = @($_.Folders)
                Groups    = @($_.Groups | ForEach-Object { [ordered]@{ Identical = $_.Identical; Files = @($_.Files) } })
            }
        })
        Incomplete      = @($result.Incomplete | ForEach-Object {
            [ordered]@{ Tag = $_.Tag; Series = $_.Series; Folder = $_.Folder; Have = $_.Have; Expected = $_.Expected; Missing = $_.Missing }
        })
        External        = @($result.External | ForEach-Object {
            [ordered]@{ Files = $_.Files; Identical = $_.Identical; Different = $_.Different; Folder = $_.Folder; OtherFolder = $_.OtherFolder }
        })
    }
    $report | ConvertTo-Json -Depth 10 -Compress | Set-Content -LiteralPath (Get-ReportPath $root) -Encoding UTF8
}

# Quarantine: identical duplicated files are moved (never deleted) to
# <parent of the counted folder>\_DicomQuarantine\<folder name>\<date_time>\<same relative path>,
# with a log to undo it. Folders named _DicomQuarantine are never counted.
function Get-QuarantineBase([string]$root) {
    $folder = $root.TrimEnd('\')
    $parent = Split-Path -Path $folder -Parent
    if (-not $parent) { return Join-Path "$folder\_DicomQuarantine" $folder.Substring(0, 1) }   # drive root
    Join-Path (Join-Path $parent '_DicomQuarantine') (Split-Path -Path $folder -Leaf)
}

# Moves every identical copy outside the set's Keep folder (one copy of each image always stays).
# Each file is compared again right before moving it, since it may have changed after the count.
function Invoke-Quarantine([string]$root, $report) {
    $folder = $root.TrimEnd('\')
    $batch = Join-Path (Get-QuarantineBase $root) (Get-Date -Format 'yyyyMMdd_HHmmss')
    $log = New-Object Collections.Generic.List[object]
    $skipped = 0; $failed = 0
    foreach ($set in $report.Duplicates) {
        foreach ($group in $set.Groups) {
            if (-not $group.Identical) { continue }
            $files = @($group.Files | Where-Object { [IO.File]::Exists($_) } | Sort-Object)
            if ($files.Count -lt 2) { continue }
            $keepFile = @($files | Where-Object { [IO.Path]::GetDirectoryName($_) -eq $set.Keep })[0]
            if (-not $keepFile) { $keepFile = $files[0] }
            foreach ($file in $files) {
                if ($file -eq $keepFile) { continue }
                if (-not $file.StartsWith($folder + '\', 'OrdinalIgnoreCase') -or
                    -not [Explorer4Dicoms.FileCompare]::Identical($keepFile, $file)) { $skipped++; continue }
                $dest = Join-Path $batch $file.Substring($folder.Length + 1)
                try {
                    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($dest)) | Out-Null
                    Move-Item -LiteralPath $file -Destination $dest
                    $log.Add([pscustomobject]@{ Original = $file; Quarantined = $dest; KeptCopy = $keepFile })
                }
                catch { $failed++ }
            }
        }
    }
    if ($log.Count) { $log | Export-Csv -LiteralPath (Join-Path $batch 'quarantine-log.csv') -NoTypeInformation -Encoding UTF8 }
    [pscustomobject]@{ Moved = $log.Count; Skipped = $skipped; Failed = $failed; Batch = $batch }
}

# Moves the files of a quarantine batch back to where they were (unless something is there again)
function Restore-Quarantine([string]$batch) {
    $logFile = Join-Path $batch 'quarantine-log.csv'
    $restored = 0; $skipped = 0
    foreach ($entry in Import-Csv -LiteralPath $logFile -Encoding UTF8) {
        if ([IO.File]::Exists($entry.Original) -or -not [IO.File]::Exists($entry.Quarantined)) { $skipped++; continue }
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($entry.Original)) | Out-Null
        Move-Item -LiteralPath $entry.Quarantined -Destination $entry.Original
        $restored++
    }
    Rename-Item -LiteralPath $logFile -NewName 'restored-log.csv'
    [pscustomobject]@{ Restored = $restored; Skipped = $skipped }
}

# Latest quarantine batch of a folder that has not been undone yet
function Get-LastQuarantine([string]$root) {
    $base = Get-QuarantineBase $root
    if (-not (Test-Path -LiteralPath $base)) { return $null }
    Get-ChildItem -LiteralPath $base -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'quarantine-log.csv') } |
        Sort-Object Name | Select-Object -Last 1
}

# Latest report covering the folder (made on it or on an ancestor), keeping only what concerns it
function Get-ProblemsReport([string]$root) {
    if (-not (Test-Path -LiteralPath $reportDir)) { return $null }
    $folder = $root.TrimEnd('\')
    $best = $null
    foreach ($file in Get-ChildItem -LiteralPath $reportDir -Filter '*.json') {
        try { $r = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json } catch { continue }
        $reportRoot = ([string]$r.Root).TrimEnd('\')
        if ($folder -ne $reportRoot -and -not $folder.StartsWith($reportRoot + '\', 'OrdinalIgnoreCase')) { continue }
        if (-not $best -or $r.ScannedAt -gt $best.ScannedAt) { $best = $r }
    }
    if (-not $best) { return $null }
    $concerns = { param($paths) @($paths | Where-Object { $_ -eq $folder -or $_.StartsWith($folder + '\', 'OrdinalIgnoreCase') }).Count -gt 0 }
    [pscustomobject]@{
        Root       = $best.Root
        ScannedAt  = $best.ScannedAt
        Splits     = @($best.Splits | Where-Object { & $concerns $_.Folders })
        Duplicates = @($best.Duplicates | Where-Object { & $concerns $_.Folders })
        Incomplete = @($best.Incomplete | Where-Object { $_ -and (& $concerns @($_.Folder)) })
        External   = @($best.External | Where-Object { $_ -and (& $concerns @($_.Folder)) })
    }
}

# Does any subfolder have a comment written by this script? (checks 500 at most)
function Test-HasDicomComments([string]$dir) {
    $n = 0
    try {
        foreach ($sub in [IO.Directory]::EnumerateDirectories($dir)) {
            if ($n++ -ge 500) { break }
            $ini = [IO.Path]::Combine($sub, 'desktop.ini')
            if ([IO.File]::Exists($ini) -and [Explorer4Dicoms.Native]::ReadIni($ini, 'explorer4dicoms', 'Generated')) { return $true }
        }
    }
    catch { }
    return $false
}

# Leaves Explorer as if the folder and its subfolders had never been counted (-Clean)
function Invoke-Clean([string]$root) {
    Stop-CountsOn $root
    # Comments, icons, folder type and the attributes added to the folders
    $cleared = 0
    foreach ($ini in Get-DesktopIniFiles $root) { if ([Explorer4Dicoms.FolderComments]::Clear($ini.DirectoryName)) { $cleared++ } }
    # The count also added the column to the parent folder: removed there too, unless other
    # folders in it still have comments
    $parent = Split-Path -Path $root -Parent
    $parentToo = $parent -and -not (Test-HasDicomComments $parent)
    Hide-CommentColumn $root -Parent:$parentToo
    $views = [Explorer4Dicoms.ViewBags]::RemoveCommentColumn($root, $true)
    if ($parentToo) { $views += [Explorer4Dicoms.ViewBags]::RemoveCommentColumn($parent, $false) }
    # Copies in other locations will no longer point here, and the reports of the counts made
    # inside the folder are deleted
    $forgotten = [Explorer4Dicoms.CountedData]::ForgetIndex($indexDir, $root)
    [void][Explorer4Dicoms.CountedData]::ForgetCopies($copiesDir, $root)
    $reports = 0
    foreach ($file in Get-ChildItem -LiteralPath $reportDir -Filter '*.json' -ErrorAction SilentlyContinue) {
        try { $reportRoot = (Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json).Root } catch { continue }
        if ($reportRoot -and [Explorer4Dicoms.Paths]::IsUnder($reportRoot, $root)) {
            Remove-Item -LiteralPath $file.FullName -Force
            $reports++
        }
    }
    Write-Host "Cleaned $root"
    Write-Host ("  Folders with comments and icons removed: {0:N0}" -f $cleared)
    Write-Host ("  Saved Explorer views without the Comments column: {0:N0}" -f $views)
    Write-Host ("  Files forgotten in the index of counted locations: {0:N0}" -f $forgotten)
    Write-Host ("  Problems reports deleted: {0:N0}" -f $reports)
}

# The folders the counts know of (their index, copies and problems reports), outermost first
function Get-CountedRoots {
    $roots = New-Object Collections.Generic.List[string]
    foreach ($dir in $indexDir, $copiesDir) {
        foreach ($file in Get-ChildItem -LiteralPath $dir -Filter '*.tsv' -ErrorAction SilentlyContinue) {
            # (their first line: "#root<TAB>folder")
            $f = @((Get-Content -LiteralPath $file.FullName -TotalCount 1 -Encoding UTF8 -ErrorAction SilentlyContinue) -split "`t")
            if ($f.Count -eq 2 -and $f[0] -eq '#root' -and $f[1]) { $roots.Add($f[1]) }
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $reportDir -Filter '*.json' -ErrorAction SilentlyContinue) {
        try { $r = (Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json).Root; if ($r) { $roots.Add($r) } } catch { }
    }
    $outer = New-Object Collections.Generic.List[string]
    foreach ($r in $roots | Sort-Object Length, { $_ } -Unique) {
        if (-not @($outer | Where-Object { [Explorer4Dicoms.Paths]::IsUnder($r, $_) }).Count) { $outer.Add($r) }
    }
    $outer
}

# Leaves the computer as before the suite (-FullClean): every counted folder cleaned, everything
# uninstalled, and its data, caches and settings deleted. Never the DICOM files, nor the quarantines
# (files the counts moved there: they are said, to restore or delete by hand).
if ($FullClean) {
    Stop-Watcher
    $roots = @(Get-CountedRoots)
    $quarantines = New-Object Collections.Generic.List[string]
    foreach ($r in $roots) {
        $base = Get-QuarantineBase $r
        if (Test-Path -LiteralPath $base) { $quarantines.Add($base) }
        if (Test-Path -LiteralPath $r -PathType Container) { Invoke-Clean $r }
        else { Write-Host "Not found now (moved, or a disk not connected), left as it is: $r" -ForegroundColor Yellow }
    }
    Invoke-Uninstall -Purge
    # Its data, caches and settings
    $programFolder = Join-Path $env:ProgramFiles $appName
    function Remove-SuiteData {
        foreach ($dir in $appData, (Join-Path ([IO.Path]::GetTempPath()) $appName)) {
            if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue }
        }
        # (the copies of the DLL in %ProgramFiles%: Register-DicomProperties.ps1 -Uninstall deletes those not in use)
        @($appData, $programFolder | Where-Object { Test-Path -LiteralPath $_ })
    }
    Remove-Item -LiteralPath "HKCU:\Software\$appName" -Recurse -Force -ErrorAction SilentlyContinue
    $left = @(Remove-SuiteData)
    # What is left is a DLL still loaded by Explorer (icons, thumbnails, preview) or the search indexer
    # (columns): Explorer restarted lets it go, if wanted
    if ($left.Count) {
        Write-Host ''
        Write-Host 'Explorer still has the DLL of the suite loaded, so it cannot be deleted yet.' -ForegroundColor Yellow
        $answer = ''
        try { $answer = Read-Host 'Restart Explorer now to release it? Its windows close and the taskbar reloads (Y/N)' } catch { }
        if ($answer -match '^\s*[yYsS]') {
            Get-Process explorer, prevhost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
            # (Windows starts the shell again by itself; if not, it is started here)
            if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
            Start-Sleep -Seconds 2
            $left = @(Remove-SuiteData)
        }
    }
    # Whatever is still there (Explorer not restarted, the search indexer, something that failed): deleted
    # now if it can be, by Windows at the next restart if not
    $atRestart = $false
    if ($left.Count) {
        Invoke-Elevated (Join-Path $PSScriptRoot 'tools\Remove-AtReboot.ps1') @('-UserData', $appData)
        . (Join-Path $PSScriptRoot 'tools\PendingDeletes.ps1')
        $left = @($left | Where-Object { Test-Path -LiteralPath $_ })
        $atRestart = $left.Count -gt 0 -and @(Get-PendingDeletes -Under $left).Count -gt 0
    }
    if (Test-Path -LiteralPath "HKLM:\SOFTWARE\$appName") { $left += "HKLM:\SOFTWARE\$appName" }
    Write-Host ''
    Write-Host ("Full clean: {0:N0} counted folders cleaned, the suite uninstalled, its data and settings deleted." -f $roots.Count)
    if ($left.Count -and $atRestart) {
        Write-Host 'Still in use, deleted by Windows at the next restart (installing again before keeps them):' -ForegroundColor Yellow
        $left | ForEach-Object { Write-Host "  $_" }
    }
    elseif ($left.Count) {
        Write-Host 'Still there (administrator rights refused): restart the computer and run -FullClean again:' -ForegroundColor Yellow
        $left | ForEach-Object { Write-Host "  $_" }
    }
    if ($quarantines.Count) {
        Write-Host 'Quarantines left as they are (files the counts moved there; -RestoreQuarantine puts a batch back):' -ForegroundColor Yellow
        $quarantines | ForEach-Object { Write-Host "  $_" }
    }
    Write-Host 'Not touched: this folder (delete it yourself), the thumbnails Windows keeps in its cache (Disk Cleanup >'
    Write-Host 'Thumbnails), and the Lister plugin if Total Commander installed it (its plugins folder and wincmd.ini).'
    return
}

if ($RestoreQuarantine) {
    $r = Restore-Quarantine $RestoreQuarantine.TrimEnd('\')
    Write-Host ("Restored {0:N0} files ({1:N0} skipped: already back or missing)" -f $r.Restored, $r.Skipped)
    return
}

if ($Watch) {
    $isFirst = $false
    $mutex = New-Object Threading.Mutex($true, 'Local\explorer4dicomsWatcher', [ref]$isFirst)
    if (-not $isFirst) { return }     # another instance is already running

    $shell = New-Object -ComObject Shell.Application
    $handled = @{}      # "hwnd|path" already checked; every tab is a separate entry
    $loops = 0
    while ($true) {
        $seen = @{}
        try {
            # ShellWindows does not enumerate reliably with foreach in PowerShell: iterate by index
            $windows = $shell.Windows()
            for ($i = 0; $i -lt $windows.Count; $i++) {
                try {
                    $window = $windows.Item($i)
                    $path = $window.Document.Folder.Self.Path
                    $key = "$($window.HWND)|$path"
                    $seen[$key] = $true
                    if ($handled.ContainsKey($key)) { continue }
                    if ([IO.Path]::IsPathRooted($path) -and (Test-HasDicomComments $path)) {
                        [void][Explorer4Dicoms.ExplorerColumns]::ShowCommentColumn($window)
                        # Folder icons lost (deleted cache, another PC): rebuilt from their names
                        try { [Explorer4Dicoms.FolderIcons]::RepairChildren($path) } catch { }
                    }
                    $handled[$key] = $true
                }
                catch { }   # view still loading: retried on the next pass
            }
        }
        catch { }
        # Forget what is no longer open, so it is checked again when returning to that folder
        foreach ($k in @($handled.Keys)) { if (-not $seen.ContainsKey($k)) { $handled.Remove($k) } }
        if ((++$loops % 600) -eq 0) { [GC]::Collect() }
        Start-Sleep -Milliseconds 500
    }
}

try {
    # From the context menu a drive root arrives as 'D:"' because of the trailing backslash escaping
    $Path = $Path.Trim().Trim('"')
    if ($Path -match '^[A-Za-z]:$') { $Path += '\' }
    # Long form: the same folder through an 8.3 short name ("JOHNDO~1") must be the same location
    $root = [Explorer4Dicoms.Paths]::Long((Resolve-Path -LiteralPath $Path).ProviderPath)

    if ($ShowDuplicates) {
        $copies = [Explorer4Dicoms.Duplicates]::Of($copiesDir, $root) | ForEach-Object { $_ -replace ' \(differs\)$', '' }
        $shown = if ($copies) { [Explorer4Dicoms.Duplicates]::Show([string[]]$copies) } else { 0 }
        if (-not $shown) {
            # Run from the context menu without a console: the answer goes in a message box
            Add-Type -AssemblyName System.Windows.Forms
            $message = if ($copies) { "The copies found by the last count no longer exist:`n`n" + ($copies -join "`n") }
                       else { "No copies of this file were found by the counts.`n`nCount the folders that may hold copies (DICOM > Count) and try again." }
            [void][Windows.Forms.MessageBox]::Show($message + "`n`n$root", 'DICOM duplicates', 'OK', 'Information')
        }
        return
    }

    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Not a folder: $root" }

    if ($ShowTree) {
        # (the tree window, lib\TreeWindow.ps1: loaded only for it)
        $mainScript = $PSCommandPath
        . (Join-Path $PSScriptRoot 'lib\TreeWindow.ps1')
        Show-DicomTree $root
        return
    }

    if ($ToggleColumns) {
        Switch-DicomColumns $root
        return
    }

    if ($Unblock) {
        $r = [Explorer4Dicoms.Unblocker]::Run($root, [Environment]::ProcessorCount)
        Write-Host ("Unblocked {0:N0} DICOM files in {1}" -f $r[0], $root)
        if ($r[1]) { Write-Host ("Left marked: {0:N0} files that are not DICOM" -f $r[1]) }
        if ($r[2]) { Write-Host ("Could not unblock {0:N0} files" -f $r[2]) -ForegroundColor Yellow }
        return
    }

    if ($Clean) {
        Invoke-Clean $root
        return
    }

    if ($WriteComments) { Show-CommentColumn $root }

    $progress = New-Object Explorer4Dicoms.ScanProgress
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $task = [Explorer4Dicoms.Scanner]::StartScan($root, [Environment]::ProcessorCount, $progress, [bool]$WriteComments, $indexDir)
    $countingFitted = $false
    while (-not $task.Wait(200)) {
        Write-Progress -Activity "Reading DICOM in $root" `
            -Status ('{0:N0} of {1:N0} files read - {2:N0} studies found' -f $progress.Files, $progress.TotalFiles, $progress.Studies)
        # Fit the width to the "Counting..." text as soon as Explorer shows it
        if ($WriteComments -and -not $countingFitted -and $progress.CountingShown) {
            Show-CommentColumn $root
            $countingFitted = $true
        }
    }
    Write-Progress -Activity "Reading DICOM in $root" -Completed
    $result = $task.Result
    $c = $result.Counts
    $sw.Stop()

    try { Save-ProblemsReport $root $result }
    catch { Write-Host "Could not save the problems report: $($_.Exception.Message)" -ForegroundColor Yellow }

    if ($WriteComments) {
        # The texts were "Counting..." at the start; now the width is refitted to the final ones
        # (after a short pause so Explorer has refreshed the values)
        Start-Sleep -Milliseconds 700
        Show-CommentColumn $root
    }

    Write-Host ''
    Write-Host "Folder: $root"
    Write-Host ''
    Write-Host ('  DISTINCT STUDIES:       {0:N0}' -f $result.Studies.Count) -ForegroundColor Green
    Write-Host ''
    Write-Host ('  DICOM files:            {0:N0}' -f $c.Dicom)
    if ($c.NoStudy -gt 0) { Write-Host ('  DICOM without StudyUID: {0:N0}' -f $c.NoStudy) }
    Write-Host ('  Non-DICOM files:        {0:N0}' -f $c.NotDicom)
    if ($c.Errors -gt 0) { Write-Host ('  Read errors:            {0:N0}' -f $c.Errors) -ForegroundColor Yellow }
    if ($c.InaccessibleDirs.Count -gt 0) {
        Write-Host ('  Inaccessible folders:   {0:N0}' -f $c.InaccessibleDirs.Count) -ForegroundColor Yellow
        $c.InaccessibleDirs | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" -ForegroundColor Yellow }
    }
    Write-Host ('  Time:                   {0:N1} s' -f $sw.Elapsed.TotalSeconds)

    if ($Detail -and $result.Studies.Count -gt 0) {
        $result.Studies |
            Format-Table StudyDate, Modalities, PatientID, StudyDescription, Series, Files, StudyInstanceUID -AutoSize |
            Out-String -Width 400 | Write-Host
    }

    if ($Csv) {
        $result.Studies | Export-Csv -LiteralPath $Csv -NoTypeInformation -Encoding UTF8 -Delimiter ';'
        Write-Host "CSV saved to $Csv"
    }

    $relative = { param($p) if ($p.Length -le $root.Length) { '(root)' } else { $p.Substring($root.Length).TrimStart('\') } }
    if ($result.Splits.Count) {
        Write-Host ''
        Write-Host ("Studies split across folders: {0}" -f $result.Splits.Count) -ForegroundColor Yellow
        foreach ($s in $result.Splits) {
            Write-Host ("  [{0}] {1}" -f $s.Tag, $s.StudyInstanceUID)
            foreach ($folder in $s.Folders) { Write-Host ("      {0}" -f (& $relative $folder)) }
        }
    }
    if ($result.Duplicates.Count) {
        Write-Host ''
        Write-Host ("Duplicated files (same StudyInstanceUID and SOPInstanceUID): {0:N0} files in {1:N0} folder sets" -f $result.DuplicatedFiles, $result.Duplicates.Count) -ForegroundColor Yellow
        foreach ($d in ($result.Duplicates | Select-Object -First 10)) {
            Write-Host ("  {0:N0} files ({1:N0} byte-identical copies, {2:N0} with different contents):" -f $d.Files, $d.Identical, $d.Different)
            foreach ($folder in $d.Folders) { Write-Host ("      {0}" -f (& $relative $folder)) }
        }
        if ($result.Duplicates.Count -gt 10) { Write-Host ("  ... and {0} more sets" -f ($result.Duplicates.Count - 10)) }
    }
    if ($result.Incomplete.Count) {
        Write-Host ''
        Write-Host ("Incomplete series: {0:N0}" -f $result.Incomplete.Count) -ForegroundColor Yellow
        foreach ($s in ($result.Incomplete | Select-Object -First 10)) {
            Write-Host ("  [{0}] {1}: {2} of {3}, missing {4}" -f $s.Tag, $s.Series, $s.Have, $s.Expected, $s.Missing)
            Write-Host ("      {0}" -f (& $relative $s.Folder))
        }
        if ($result.Incomplete.Count -gt 10) { Write-Host ("  ... and {0} more" -f ($result.Incomplete.Count - 10)) }
    }
    if ($result.External.Count) {
        $externalFiles = ($result.External | Measure-Object -Property Files -Sum).Sum
        Write-Host ''
        Write-Host ("Also in other counted locations: {0:N0} files in {1:N0} folder pairs" -f $externalFiles, $result.External.Count) -ForegroundColor Yellow
        foreach ($x in ($result.External | Select-Object -First 10)) {
            Write-Host ("  {0:N0} files ({1:N0} byte-identical, {2:N0} with different contents):" -f $x.Files, $x.Identical, $x.Different)
            Write-Host ("      {0}" -f (& $relative $x.Folder))
            Write-Host ("      {0}" -f $x.OtherFolder)
        }
        if ($result.External.Count -gt 10) { Write-Host ("  ... and {0} more" -f ($result.External.Count - 10)) }
    }

    if ($WriteComments) {
        $withStudies = @($result.Folders | Where-Object { $_.Studies -gt 0 }).Count
        Write-Host ''
        Write-Host ("Comments written to {0:N0} folders with DICOM" -f $withStudies) -ForegroundColor Green
        if ($c.CommentErrors -gt 0) { Write-Host ("Folders that could not be written: {0:N0}" -f $c.CommentErrors) -ForegroundColor Yellow }
    }
}
catch {
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    if ($Pause) {
        Write-Host ''
        Write-Host 'Press any key to close...'
        [void][Console]::ReadKey($true)
    }
}
