<#
.SYNOPSIS
    Counts the distinct DICOM studies (StudyInstanceUID) in a folder and all its subfolders.

.DESCRIPTION
    Reads only the header of each file (it stops at InstanceNumber, tag 0020,0013), so pixel
    data is never loaded. Supports Explicit/Implicit VR Little Endian, Explicit VR Big Endian,
    Deflated and "raw" DICOM files without preamble. Non-DICOM files are ignored.

.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -Detail
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -Csv C:\temp\studies.csv
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -WriteComments    # fills Explorer's Comments column
    .\Count-DicomStudies.ps1 D:\Patients -Clean            # as if it had never been counted
.EXAMPLE
    .\Count-DicomStudies.ps1 D:\Patients -ShowTree         # folder tree with the comments
.EXAMPLE
    .\Count-DicomStudies.ps1 -RestoreQuarantine D:\_DicomQuarantine\Patients\20261022_091500
.EXAMPLE
    .\Count-DicomStudies.ps1 -Install      # context menu + background watcher (-Watch) at logon
    .\Count-DicomStudies.ps1 -Uninstall
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

    # Same as -Clean (older name)
    [Parameter(ParameterSetName = 'Scan')]
    [switch]$ClearComments,

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

    [Parameter(ParameterSetName = 'Uninstall', Mandatory = $true)]
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# Context menu: a "DICOM" submenu with fixed entries (the menu cannot depend on whether the folder
# was counted: Explorer does not read desktop.ini when it builds it). Key names set the order.
$menuName = 'DicomStudyCount'
$menuEntries = @(
    @{ Name = '1Count'; Label = 'Count studies'; Args = '-WriteComments' },
    @{ Name = '2Tree'; Label = 'Show tree'; Args = '-ShowTree' },
    @{ Name = '3Clean'; Label = 'Clean'; Args = '-Clean' },
    @{ Name = '4Columns'; Label = 'Show/hide DICOM columns'; Args = '-ToggleColumns' },
    @{ Name = '5Unblock'; Label = 'Unblock (allow preview)'; Args = '-Unblock' }
)
# The DICOM columns of the files (bin\DicomStudyCount.propdesc), in the order they are shown
$dicomColumns = 'StudyHash', 'SeriesHash', 'Modality', 'SeriesNumber', 'InstanceNumber', 'SeriesDescription',
    'Plane', 'Acquired', 'TriggerTime', 'ZLevel', 'Rows', 'Columns', 'PixelSpacing', 'SliceThickness', 'Frames',
    'ImageType', 'ObjectType', 'Compression', 'Protocol', 'Manufacturer', 'FrameOfReference', 'StudyDate',
    'PatientID', 'SensitiveData', 'ImageOrientation', 'StudyInstanceUID', 'SeriesInstanceUID', 'DuplicatedIn' |
    ForEach-Object { "DicomStudyCount.$_" }
$menuParents = @(
    'HKCU:\Software\Classes\Directory\shell',
    'HKCU:\Software\Classes\Directory\Background\shell'
)
# Entries of earlier versions, directly in the context menu
$oldMenuNames = 'CountDicomStudies', 'ShowDicomTree', 'CleanDicomInfo'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'DicomStudyCountWatcher'
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
        Where-Object { $_.CommandLine -like '*Count-DicomStudies.ps1*-Watch*' -and $_.ProcessId -ne $PID } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

if ($Install) {
    Set-ItemProperty -LiteralPath $runKey -Name $runName -Value $watchCommand
    Stop-Watcher
    Start-Process conhost.exe -ArgumentList ($watchCommand -replace '^conhost\.exe ', '')
    foreach ($parent in $menuParents) {
        foreach ($old in $oldMenuNames) {
            if (Test-Path -LiteralPath "$parent\$old") { Remove-Item -LiteralPath "$parent\$old" -Recurse -Force }
        }
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
            New-Item -Path "$key\command" -Force | Out-Null
            Set-ItemProperty -LiteralPath $key -Name 'MUIVerb' -Value $entry.Label
            Set-ItemProperty -LiteralPath "$key\command" -Name '(default)' -Value $command
        }
    }
    # Icons of the DICOM files (bin\DicomShell.dll, registered for the user)
    $shellDll = Join-Path $PSScriptRoot 'bin\DicomShell.dll'
    & (Join-Path $PSScriptRoot 'tools\Register-DicomIcons.ps1') -Dll $shellDll -Script $PSCommandPath
    # The columns of the DICOM files: Windows takes property handlers from HKLM only. No UAC prompt
    # when the DLL and the column schema installed are already these ones.
    $sha = [Security.Cryptography.SHA256]::Create()
    $dllHash = -join ($sha.ComputeHash([IO.File]::ReadAllBytes($shellDll))[0..7] | ForEach-Object { $_.ToString('x2') })
    $server = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Classes\CLSID\{21329A2B-09B3-4250-9103-9267877321EB}\InprocServer32')
    $installedSchema = Join-Path $env:ProgramFiles 'DicomStudyCount\DicomStudyCount.propdesc'
    $handlers = 'SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers'
    $upToDate = $server -and [IO.Path]::GetFileName([string]$server.GetValue('')) -eq "DicomShell-$dllHash.dll" -and
        @('.', '.dcm', '.ima', '.dicom' | Where-Object { -not [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$handlers\$_") }).Count -eq 0 -and
        (Test-Path -LiteralPath $installedSchema) -and
        ((Get-FileHash -LiteralPath $installedSchema).Hash -eq (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'bin\DicomStudyCount.propdesc')).Hash)
    if ($upToDate) { Write-Host 'DICOM columns already installed and up to date' }
    else { Invoke-Elevated (Join-Path $PSScriptRoot 'tools\Register-DicomProperties.ps1') @('-Dll', $shellDll) }
    # The C# DLL used to be compiled by the script itself into this cache: no longer used
    $oldCache = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\bin'
    if (Test-Path -LiteralPath $oldCache) { Remove-Item -LiteralPath $oldCache -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host 'Installed. Right-click a folder: DICOM >'
    $menuEntries | ForEach-Object { Write-Host "  - $($_.Label)" }
    Write-Host '(on Windows 11 it is under "Show more options" when using the new context menu).'
    Write-Host 'Background watcher running and registered to start at logon.'
    return
}

if ($Uninstall) {
    Remove-ItemProperty -LiteralPath $runKey -Name $runName -ErrorAction SilentlyContinue
    Stop-Watcher
    foreach ($parent in $menuParents) {
        foreach ($name in @($menuName) + $oldMenuNames) {
            if (Test-Path -LiteralPath "$parent\$name") { Remove-Item -LiteralPath "$parent\$name" -Recurse -Force }
        }
    }
    & (Join-Path $PSScriptRoot 'tools\Register-DicomIcons.ps1') -Uninstall
    if ([Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\DicomStudyCount\PropertyHandler')) {
        Invoke-Elevated (Join-Path $PSScriptRoot 'tools\Register-DicomProperties.ps1') @('-Uninstall')
    }
    Write-Host 'Uninstalled.'
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
            if ([DicomStudyCount.Paths]::IsUnder($windowPath, $root) -or ($Parent -and $windowPath -eq $parentPath)) {
                [void][DicomStudyCount.ExplorerColumns]::HideCommentColumn($window)
            }
        }
        catch { }   # windows that are not file system folders (This PC, Home...)
    }
}

# Stops the counts running on the folder, inside it, or on a folder that contains it: they would
# write their comments again over the cleaned folders
function Stop-CountsOn([string]$root) {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.CommandLine -like '*Count-DicomStudies.ps1*-WriteComments*' -and $_.ProcessId -ne $PID } |
        ForEach-Object {
            # Counts are started with -Path "<folder>" -WriteComments (conhost --headless drops the
            # quotes, so the folder is whatever lies between both parameters)
            if ($_.CommandLine -notmatch '-Path\s+(.+?)\s+-WriteComments\b') { return }
            $p = $Matches[1].Trim().Trim('"').TrimEnd('\')
            if ($p -match '^[A-Za-z]:$') { $p += '\' }
            if ([DicomStudyCount.Paths]::IsUnder($p, $root) -or [DicomStudyCount.Paths]::IsUnder($root, $p)) {
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
    $show = -not [DicomStudyCount.ExplorerColumns]::AnyColumnShown($targets[0], [string[]]$dicomColumns)
    foreach ($w in $targets) {
        try { [DicomStudyCount.ExplorerColumns]::SetColumnsShown($w, [string[]]$dicomColumns, $show) } catch { }
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
                [void][DicomStudyCount.ExplorerColumns]::ShowCommentColumn($window)
            }
        }
        catch { }   # windows that are not file system folders (This PC, Home...)
    }
}

# The C# part (DICOM reader, count, folder comments and icons, Explorer columns): bin\DicomStudyCount.dll,
# built from src\DicomStudyCount.cs by tools\Build.ps1. Loaded from its bytes, so the file is never
# locked: the folder can be updated while the background watcher runs.
if (-not ('DicomStudyCount.Scanner' -as [type])) {
    $assembly = Join-Path $PSScriptRoot 'bin\DicomStudyCount.dll'
    if (-not (Test-Path -LiteralPath $assembly)) { throw "Not found: $assembly (build it with tools\Build.ps1)" }
    Add-Type -AssemblyName System.Drawing
    [void][Reflection.Assembly]::Load([IO.File]::ReadAllBytes($assembly))
}

# Problems reports (split studies, duplicated files) live outside the scanned folders,
# one JSON file per scanned root
$reportDir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\reports'
# Index of counted locations, to find copies across them (one file per counted folder)
$indexDir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\index'
# Copies of every file found by the counts, for the "DICOM duplicated in" column (written by the
# counts next to the index, read by the property handler of bin\DicomShell.dll)
$copiesDir = Join-Path $env:LOCALAPPDATA 'DicomStudyCount\duplicates'

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
                    -not [DicomStudyCount.FileCompare]::Identical($keepFile, $file)) { $skipped++; continue }
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
            if ([IO.File]::Exists($ini) -and [DicomStudyCount.Native]::ReadIni($ini, 'DicomStudyCount', 'Generated')) { return $true }
        }
    }
    catch { }
    return $false
}

# Window with the folder tree and the comment of every folder next to it. Reads desktop.ini only:
# it neither reads DICOM files nor writes anything.
function Show-DicomTree([string]$root) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    [void][DicomStudyCount.Native]::SetProcessDPIAware()
    [Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object Windows.Forms.Form
    $form.Text = "DICOM tree - $root"
    $form.Size = New-Object Drawing.Size(1250, 800)
    $form.StartPosition = 'CenterScreen'
    $form.Font = New-Object Drawing.Font('Segoe UI', 9)
    $form.KeyPreview = $true

    $tree = New-Object Windows.Forms.TreeView
    $tree.Dock = 'Fill'
    $tree.DrawMode = 'OwnerDrawText'
    $tree.HideSelection = $false
    $tree.ShowNodeToolTips = $true
    $tree.ItemHeight = [int]($form.Font.Height * 1.6)

    # Folder icons: the same ones Explorer shows (study colors), at the size of the screen's DPI
    $dpiGraphics = $tree.CreateGraphics()
    $script:iconSize = [int][Math]::Round(16 * $dpiGraphics.DpiX / 96)
    $dpiGraphics.Dispose()
    $images = New-Object Windows.Forms.ImageList
    $images.ColorDepth = 'Depth32Bit'
    $images.ImageSize = New-Object Drawing.Size($script:iconSize, $script:iconSize)
    try { $images.Images.Add('folder', [DicomStudyCount.FolderIcons]::SystemFolder($script:iconSize)) } catch { }
    $tree.ImageList = $images
    $tree.ImageKey = 'folder'
    $tree.SelectedImageKey = 'folder'

    $toolbar = New-Object Windows.Forms.FlowLayoutPanel
    $toolbar.Dock = 'Top'
    $toolbar.AutoSize = $true
    $toolbar.Padding = New-Object Windows.Forms.Padding(4)
    $script:scriptPath = $PSCommandPath
    $countRoot = { Start-Process conhost.exe -ArgumentList ('--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "{1}" -WriteComments' -f $script:scriptPath, $root) }
    $expandToStudies = {
        if (-not $tree.Nodes.Count) { return }
        $tree.BeginUpdate()
        Expand-ToStudies $tree.Nodes[0]
        $tree.EndUpdate()
        $tree.Nodes[0].EnsureVisible()
    }
    foreach ($b in @(@('Recount', $countRoot),
                     @('Expand to studies', $expandToStudies),
                     @('Expand all', { $tree.ExpandAll(); if ($tree.Nodes.Count) { $tree.Nodes[0].EnsureVisible() } }),
                     @('Collapse all', { $tree.CollapseAll(); if ($tree.Nodes.Count) { $tree.Nodes[0].Expand() } }),
                     @('Refresh (F5)', { Update-Tree }),
                     @('Problems', { Show-Problems }))) {
        $button = New-Object Windows.Forms.Button
        $button.Text = $b[0]
        $button.AutoSize = $true
        $button.add_Click($b[1])
        $toolbar.Controls.Add($button)
        if ($b[0] -eq 'Problems') { $problemsButton = $button }
    }

    # Updates the Problems button with the number of problems in the latest report
    function Update-ProblemsButton {
        $report = Get-ProblemsReport $root
        $count = if ($report) { $report.Splits.Count + $report.Duplicates.Count + $report.Incomplete.Count + $report.External.Count } else { 0 }
        $problemsButton.Text = if ($count) { "Problems ($count)" } else { 'Problems' }
        $problemsButton.ForeColor = if ($count) { [Drawing.Color]::FromArgb(180, 0, 0) } else { [Drawing.SystemColors]::ControlText }
    }

    # The quarantine of identical duplicates (Invoke-Quarantine / Restore-Quarantine) is implemented but
    # its interface is disabled for now: the Problems window only reports. Set to $true to show the
    # quarantine buttons and the right-click "Keep the copies in this folder".
    $script:quarantineEnabled = $false

    # Folder nodes of a duplicate set: the folder suggested to keep the copies is marked in bold green
    function Update-SetNode($setNode) {
        $set = $setNode.Tag
        $mark = if ($script:quarantineEnabled) { '(keep)' } else { '(suggested keep)' }
        foreach ($child in $setNode.Nodes) {
            if ($child.Tag -eq $set.Keep) {
                $child.Text = "$mark  $($child.Tag)"
                $child.NodeFont = New-Object Drawing.Font($tree.Font, [Drawing.FontStyle]::Bold)
                $child.ForeColor = [Drawing.Color]::FromArgb(0, 120, 40)
            } else {
                $child.Text = $child.Tag
                $child.NodeFont = $null
                $child.ForeColor = [Drawing.Color]::Empty
            }
        }
    }

    $quarantineClick = {
        $report = $script:problemsReport
        $toMove = 0
        foreach ($set in $report.Duplicates) {
            foreach ($g in $set.Groups) { if ($g.Identical) { $toMove += @($g.Files).Count - 1 } }
        }
        if (-not $toMove) {
            [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, 'There are no identical copies to quarantine.', 'Quarantine')
            return
        }
        $message = "Move $toMove identical copies to:`n$(Get-QuarantineBase $root)\<date_time>`n`n" +
                   "One copy of each image stays, in the folders marked (keep).`n" +
                   "Files with the same SOPInstanceUID but different contents are not touched.`n" +
                   "Nothing is deleted: 'Undo last quarantine' moves them back."
        $answer = [Windows.Forms.MessageBox]::Show($script:problemsDialog, $message, 'Quarantine identical duplicates', 'YesNo', 'Warning')
        if ($answer -ne 'Yes') { return }
        $script:problemsDialog.Cursor = 'WaitCursor'
        $r = Invoke-Quarantine $root $report
        $script:problemsDialog.Cursor = 'Default'
        [void][Windows.Forms.MessageBox]::Show($script:problemsDialog,
            ("Moved: {0}`nSkipped (changed since the count, or outside this folder): {1}`nFailed: {2}`n`nQuarantine folder:`n{3}`n`nThe folder is counted again now to update the comments." -f $r.Moved, $r.Skipped, $r.Failed, $r.Batch),
            'Quarantine')
        $script:problemsDialog.Close()
        & $countRoot
    }

    $undoClick = {
        $last = Get-LastQuarantine $root
        if (-not $last) {
            [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, 'There is no quarantine to undo for this folder.', 'Undo')
            return
        }
        $answer = [Windows.Forms.MessageBox]::Show($script:problemsDialog,
            "Move back the files quarantined in:`n$($last.FullName)?", 'Undo last quarantine', 'YesNo', 'Question')
        if ($answer -ne 'Yes') { return }
        $r = Restore-Quarantine $last.FullName
        [void][Windows.Forms.MessageBox]::Show($script:problemsDialog,
            ("Restored: {0}`nSkipped (already back or missing): {1}`n`nThe folder is counted again now to update the comments." -f $r.Restored, $r.Skipped),
            'Undo last quarantine')
        $script:problemsDialog.Close()
        & $countRoot
    }

    # Window with the latest problems report: studies split across folders and duplicated files,
    # with the quarantine of identical copies. Clicking a folder selects it in the tree,
    # double-clicking opens it in Explorer, right-clicking a folder of a duplicate set chooses it as
    # the one that keeps the copies. (Modeless: what its events need is kept in script variables.)
    function Show-Problems {
        $report = Get-ProblemsReport $root
        if (-not $report) {
            [void][Windows.Forms.MessageBox]::Show($form, 'There is no report for this folder yet: click "Recount" first.', 'Problems')
            return
        }
        if ($script:problemsDialog -and -not $script:problemsDialog.IsDisposed) { $script:problemsDialog.Close() }
        $script:problemsReport = $report

        $dialog = New-Object Windows.Forms.Form
        $dialog.Text = "Problems - $root  (counted $($report.ScannedAt -replace 'T', ' '))"
        $dialog.Size = New-Object Drawing.Size(1150, 700)
        $dialog.StartPosition = 'CenterParent'
        $dialog.Font = $form.Font
        $script:problemsDialog = $dialog

        $bar = New-Object Windows.Forms.FlowLayoutPanel
        $bar.Dock = 'Top'
        $bar.AutoSize = $true
        $bar.Padding = New-Object Windows.Forms.Padding(4)
        foreach ($b in @(@('Quarantine identical duplicates...', $quarantineClick),
                         @('Undo last quarantine', $undoClick),
                         @('Open quarantine folder', {
                             $base = Get-QuarantineBase $root
                             if (Test-Path -LiteralPath $base) { Start-Process explorer.exe ('"{0}"' -f $base) }
                             else { [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, "Nothing quarantined yet.`n($base)", 'Quarantine') }
                         }))) {
            $button = New-Object Windows.Forms.Button
            $button.Text = $b[0]
            $button.AutoSize = $true
            $button.add_Click($b[1])
            $bar.Controls.Add($button)
        }
        $bar.Visible = $script:quarantineEnabled

        $list = New-Object Windows.Forms.TreeView
        $list.Dock = 'Fill'
        $list.HideSelection = $false
        $list.ItemHeight = $tree.ItemHeight

        $splits = $list.Nodes.Add("Studies split across folders ($($report.Splits.Count))")
        foreach ($s in $report.Splits) {
            $sn = $splits.Nodes.Add("[$($s.Tag)] $($s.StudyInstanceUID)  -  in $(@($s.Folders).Count) folders")
            foreach ($folder in $s.Folders) { $fn = $sn.Nodes.Add($folder); $fn.Tag = $folder }
        }
        $dupFiles = [int]($report.Duplicates | Measure-Object -Property Files -Sum).Sum
        $dupsLabel = "Duplicated files, same StudyInstanceUID and SOPInstanceUID ($dupFiles files in $($report.Duplicates.Count) folder sets)"
        if ($script:quarantineEnabled) { $dupsLabel += '  -  right-click a folder to keep the copies there' }
        $dups = $list.Nodes.Add($dupsLabel)
        foreach ($d in $report.Duplicates) {
            $where = if (@($d.Folders).Count -eq 1) { 'inside one folder' } else { "shared by $(@($d.Folders).Count) folders" }
            $dn = $dups.Nodes.Add("$($d.Files) files $where  -  $($d.Identical) byte-identical, $($d.Different) with different contents")
            if ($d.Different) { $dn.ForeColor = [Drawing.Color]::FromArgb(180, 0, 0) }
            $dn.Tag = $d
            foreach ($folder in $d.Folders) { $fn = $dn.Nodes.Add($folder); $fn.Tag = $folder }
            Update-SetNode $dn
        }
        $incomplete = $list.Nodes.Add("Incomplete series ($($report.Incomplete.Count))  -  InstanceNumbers missing in a folder, compared with the whole tree")
        foreach ($s in $report.Incomplete) {
            $sn = $incomplete.Nodes.Add("[$($s.Tag)] $($s.Series)  -  $($s.Have) of $($s.Expected), missing $($s.Missing)")
            $fn = $sn.Nodes.Add($s.Folder); $fn.Tag = $s.Folder
        }
        $externalFiles = [int]($report.External | Measure-Object -Property Files -Sum).Sum
        $external = $list.Nodes.Add("Also in other counted locations ($externalFiles files in $($report.External.Count) folder pairs)")
        foreach ($x in $report.External) {
            $xn = $external.Nodes.Add("$($x.Files) files  -  $($x.Identical) byte-identical, $($x.Different) with different contents")
            if ($x.Different) { $xn.ForeColor = [Drawing.Color]::FromArgb(180, 0, 0) }
            $fn = $xn.Nodes.Add($x.Folder); $fn.Tag = $x.Folder
            $on = $xn.Nodes.Add("elsewhere:  $($x.OtherFolder)"); $on.Tag = $x.OtherFolder
        }
        $splits.ExpandAll()
        $dups.ExpandAll()
        $incomplete.ExpandAll()
        $external.ExpandAll()
        if ($list.Nodes.Count) { $list.Nodes[0].EnsureVisible() }

        $menu = New-Object Windows.Forms.ContextMenuStrip
        $keepItem = $menu.Items.Add('Keep the copies in this folder')
        $keepItem.add_Click({
            $node = $script:menuNode
            $node.Parent.Tag.Keep = $node.Tag
            Update-SetNode $node.Parent
        })
        $list.add_NodeMouseClick({
            param($sender, $e)
            if ($e.Button -ne 'Right' -or -not $script:quarantineEnabled) { return }
            $sender.SelectedNode = $e.Node
            # Only folders of a duplicate set (their parent holds the set, which has Groups)
            if ($e.Node.Tag -is [string] -and $e.Node.Parent -and $e.Node.Parent.Tag -and $e.Node.Parent.Tag.PSObject.Properties['Groups']) {
                $script:menuNode = $e.Node
                $script:problemsMenu.Show($sender, $e.Location)
            }
        })
        $script:problemsMenu = $menu
        $list.add_AfterSelect({
            param($sender, $e)
            $node = if ($e.Node.Tag -is [string]) { $script:nodeByPath[$e.Node.Tag] } else { $null }
            if ($node) { $tree.SelectedNode = $node; $node.EnsureVisible() }
        })
        $list.add_NodeMouseDoubleClick({
            param($sender, $e)
            if ($e.Node.Tag -is [string]) { Start-Process explorer.exe ('"{0}"' -f $e.Node.Tag) }
        })
        $dialog.Controls.Add($list)
        $dialog.Controls.Add($bar)
        $dialog.Show($form)
    }

    $statusStrip = New-Object Windows.Forms.StatusStrip
    $statusLabel = New-Object Windows.Forms.ToolStripStatusLabel
    [void]$statusStrip.Items.Add($statusLabel)

    $form.Controls.Add($tree)
    $form.Controls.Add($toolbar)
    $form.Controls.Add($statusStrip)

    # Natural order: "2" before "10"
    $naturalKey = { [regex]::Replace([IO.Path]::GetFileName($_), '\d+', { $args[0].Value.PadLeft(12, '0') }) }

    function Get-FolderComment([string]$dir) {
        $ini = [IO.Path]::Combine($dir, 'desktop.ini')
        if (-not [IO.File]::Exists($ini)) { return '' }
        $text = [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo', 'InfoTip')
        # Non-ASCII text (such as the warning sign) is also stored UTF-7 encoded in .ShellClassInfo.W,
        # valid only while the .ShellClassInfo.A copy matches the main text (same rule as Windows)
        $unicode = [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo.W', 'InfoTip')
        if ($unicode -and [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo.A', 'InfoTip') -eq $text) {
            return [Text.Encoding]::UTF7.GetString([Text.Encoding]::ASCII.GetBytes($unicode))
        }
        $text
    }

    # Shows the folder icon written by the count (rebuilt if it was lost), or the plain folder
    function Set-NodeIcon($node) {
        $key = 'folder'
        try {
            $icon = [DicomStudyCount.FolderIcons]::OfFolder($node.Tag.Path)
            if ($icon) {
                if (-not $images.Images.ContainsKey($icon)) { $images.Images.Add($icon, [DicomStudyCount.FolderIcons]::Load($icon, $script:iconSize)) }
                $key = $icon
            }
        }
        catch { }
        if ($node.ImageKey -ne $key) {
            $node.ImageKey = $key
            $node.SelectedImageKey = $key
        }
    }

    # Files directly in the folder (the desktop.ini written by this script does not count)
    function Get-DirectFileCount([string]$dir) {
        try { $n = [IO.Directory]::GetFiles($dir).Length } catch { return 0 }
        if ([IO.File]::Exists([IO.Path]::Combine($dir, 'desktop.ini'))) { $n-- }
        $n
    }

    # Adds the folder and its whole subtree and returns its node; $index inserts it at a
    # position instead of at the end. TotalFiles counts the files of the whole subtree.
    function Add-FolderNode($nodes, [string]$dir, [string]$name, [int]$level, [int]$index = -1) {
        $comment = Get-FolderComment $dir
        $direct = Get-DirectFileCount $dir
        $node = New-Object Windows.Forms.TreeNode($name)
        if ($index -ge 0) { $nodes.Insert($index, $node) } else { [void]$nodes.Add($node) }
        $node.Tag = [pscustomobject]@{ Path = $dir; Comment = $comment; StudyUid = (Get-StudyUid $comment); DirectFiles = $direct; TotalFiles = $direct }
        $node.ToolTipText = $comment
        Set-NodeIcon $node
        $script:nodeByPath[$dir] = $node
        $width = ($level + 1) * $tree.Indent + $script:iconSize + 4 + [Windows.Forms.TextRenderer]::MeasureText($name, $tree.Font).Width
        if ($width -gt $script:nameWidth) { $script:nameWidth = $width }
        $subs = @()
        try { $subs = [IO.Directory]::GetDirectories($dir) | Sort-Object $naturalKey } catch { }
        foreach ($sub in $subs) {
            $child = Add-FolderNode $node.Nodes $sub ([IO.Path]::GetFileName($sub)) ($level + 1)
            $node.Tag.TotalFiles += $child.Tag.TotalFiles
        }
        $node
    }

    function Remove-FromIndex($node) {
        $script:nodeByPath.Remove($node.Tag.Path)
        foreach ($child in $node.Nodes) { Remove-FromIndex $child }
    }

    function Repaint-Row($node) {
        if ($node.IsVisible) {
            $b = $node.Bounds
            $tree.Invalidate((New-Object Drawing.Rectangle(0, $b.Y, $tree.ClientSize.Width, $b.Height)))
        }
    }

    # Adds a change in the number of files to the folder and all its ancestors
    function Add-FileDelta($node, [int]$delta) {
        if ($delta -eq 0) { return }
        for ($n = $node; $n; $n = $n.Parent) {
            $n.Tag.TotalFiles += $delta
            Repaint-Row $n
        }
    }

    # Makes the children of a node match the subfolders currently on disk (same natural order)
    function Sync-Children($node) {
        $subs = @()
        try { $subs = @([IO.Directory]::GetDirectories($node.Tag.Path) | Sort-Object $naturalKey) } catch { }
        $onDisk = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($s in $subs) { [void]$onDisk.Add($s) }
        foreach ($child in @($node.Nodes)) {
            if (-not $onDisk.Contains($child.Tag.Path)) {
                Remove-FromIndex $child
                $child.Remove()
                Add-FileDelta $node (-$child.Tag.TotalFiles)
            }
        }
        for ($i = 0; $i -lt $subs.Count; $i++) {
            if ($i -lt $node.Nodes.Count -and $node.Nodes[$i].Tag.Path -eq $subs[$i]) { continue }
            $added = Add-FolderNode $node.Nodes $subs[$i] ([IO.Path]::GetFileName($subs[$i])) ($node.Level + 1) $i
            Add-FileDelta $node $added.Tag.TotalFiles
        }
    }

    # Single-study folders have comments starting with their study tag, "[CT-xxxxx] dd/MMM/yyyy ...";
    # the tag identifies the study in the tree (colors, "Expand to studies")
    function Get-StudyUid([string]$comment) {
        # 6 characters; 5 in the comments written by earlier versions, until counted again
        if ($comment -cmatch '^\[([A-Z0-9]+-[0-9A-Za-z]{5,6})\] ') { return $Matches[1] }
        $null
    }

    # Soft pastel color for the i-th study: the hue advances by the golden angle, so consecutive
    # studies always get clearly different hues; lightness is high enough for black text.
    function Get-PastelColor([int]$index) {
        $h = (($index * 137.508) % 360) / 360.0
        $s = 0.70; $l = 0.86
        $q = $l + $s - $l * $s
        $p = 2 * $l - $q
        $rgb = foreach ($t in ($h + 1 / 3), $h, ($h - 1 / 3)) {
            if ($t -lt 0) { $t += 1 }
            if ($t -gt 1) { $t -= 1 }
            $v = if ($t -lt 1 / 6) { $p + ($q - $p) * 6 * $t }
                 elseif ($t -lt 1 / 2) { $q }
                 elseif ($t -lt 2 / 3) { $p + ($q - $p) * (2 / 3 - $t) * 6 }
                 else { $p }
            [int][Math]::Round($v * 255)
        }
        [Drawing.Color]::FromArgb($rgb[0], $rgb[1], $rgb[2])
    }

    # Assigns a color to every study in tree order (pre-order, natural sort), so the same folder
    # always gets the same colors; repaints only if the assignment changed
    function Update-StudyColors {
        $order = New-Object Collections.Generic.List[string]
        $seen = New-Object 'System.Collections.Generic.HashSet[string]'
        $stack = New-Object System.Collections.Stack
        for ($i = $tree.Nodes.Count - 1; $i -ge 0; $i--) { $stack.Push($tree.Nodes[$i]) }
        while ($stack.Count) {
            $n = $stack.Pop()
            $uid = $n.Tag.StudyUid
            if ($uid -and $seen.Add($uid)) { $order.Add($uid) }
            for ($i = $n.Nodes.Count - 1; $i -ge 0; $i--) { $stack.Push($n.Nodes[$i]) }
        }
        $current = if ($script:studyOrder) { $script:studyOrder -join '|' } else { '' }
        if (($order -join '|') -eq $current) { return }
        $script:studyOrder = $order.ToArray()
        if ($script:studyBrushes) { foreach ($b in $script:studyBrushes.Values) { $b.Dispose() } }
        $script:studyBrushes = New-Object Collections.Hashtable     # case-sensitive: tags differ by case
        for ($i = 0; $i -lt $order.Count; $i++) {
            $script:studyBrushes[$order[$i]] = New-Object Drawing.SolidBrush (Get-PastelColor $i)
        }
        $tree.Invalidate()
    }

    # Expands folders holding several studies and stops at the first single-study level;
    # folders without DICOM stay collapsed
    function Expand-ToStudies($node) {
        if ($node.Tag.StudyUid -or $node.Tag.Comment -notmatch '^\d+ DICOM studies') {
            $node.Collapse()
            return
        }
        $node.Expand()
        foreach ($child in $node.Nodes) { Expand-ToStudies $child }
    }

    function Update-Status {
        $commented = 0
        foreach ($n in $script:nodeByPath.Values) { if ($n.Tag.Comment) { $commented++ } }
        $rootComment = if ($tree.Nodes.Count) { $tree.Nodes[0].Tag.Comment } else { '' }
        $files = if ($tree.Nodes.Count) { '{0:N0} files  -  ' -f $tree.Nodes[0].Tag.TotalFiles } else { '' }
        if ($script:studyOrder -and $script:studyOrder.Count) { $files += '{0} studies  -  ' -f $script:studyOrder.Count }
        $statusLabel.Text = $files + $(if ($rootComment -like 'Counting*') {
            "Counting... the tree updates as each folder is finished"
        } elseif ($commented) {
            "$commented folders with comments  -  double-click: open in Explorer  -  F5: refresh"
        } else {
            'No comments found: click "Recount" to count this folder'
        })
    }

    # Rebuilds the tree keeping the expanded folders and the selection
    function Update-Tree {
        $expanded = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $stack = New-Object System.Collections.Stack
        foreach ($n in $tree.Nodes) { $stack.Push($n) }
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($n.IsExpanded) { [void]$expanded.Add($n.Tag.Path) }
            foreach ($c in $n.Nodes) { $stack.Push($c) }
        }
        $selected = if ($tree.SelectedNode) { $tree.SelectedNode.Tag.Path } else { $null }

        $form.Cursor = 'WaitCursor'
        $tree.BeginUpdate()
        $tree.Nodes.Clear()
        $script:nodeByPath = New-Object Collections.Hashtable ([StringComparer]::OrdinalIgnoreCase)
        $script:nameWidth = 0
        [void](Add-FolderNode $tree.Nodes $root $root 0)
        # Common columns right after the widest folder name: number of files, then the comment
        $script:filesX = [Math]::Min($script:nameWidth + 30, 560)
        $script:filesW = [Windows.Forms.TextRenderer]::MeasureText('9,999,999 files', $tree.Font).Width
        $script:commentX = $script:filesX + $script:filesW + 24

        $stack.Clear()
        foreach ($n in $tree.Nodes) { $stack.Push($n) }
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($expanded.Contains($n.Tag.Path)) { $n.Expand() }
            if ($selected -and $n.Tag.Path -eq $selected) { $tree.SelectedNode = $n }
            foreach ($c in $n.Nodes) { $stack.Push($c) }
        }
        if ($expanded.Count -eq 0) { $tree.Nodes[0].Expand() }
        $tree.EndUpdate()
        $form.Cursor = 'Default'
        Update-StudyColors
        Update-Status
        Update-ProblemsButton
    }

    $tree.add_DrawNode({
        param($sender, $e)
        if ($e.Bounds.IsEmpty) { return }
        $selected = ($e.State -band [Windows.Forms.TreeNodeStates]::Selected) -ne 0
        $flags = [Windows.Forms.TextFormatFlags]'VerticalCenter, Left, NoPrefix, SingleLine'
        $nameColor = $tree.ForeColor
        # Single-study rows: pastel background with the color of their study, from the name to the edge
        $uid = $e.Node.Tag.StudyUid
        $studyBrush = if ($uid -and $script:studyBrushes) { $script:studyBrushes[$uid] } else { $null }
        if ($studyBrush) {
            $e.Graphics.FillRectangle($studyBrush, $e.Bounds.X, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $e.Bounds.X), $e.Bounds.Height)
        }
        if ($selected) {
            $e.Graphics.FillRectangle([Drawing.SystemBrushes]::Highlight, $e.Bounds)
            $nameColor = [Drawing.SystemColors]::HighlightText
        }
        [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $e.Node.Text, $tree.Font, $e.Bounds, $nameColor, $flags)

        # Number of files, right-aligned in its own column
        $total = $e.Node.Tag.TotalFiles
        $filesText = if ($total -eq 1) { '1 file' } else { '{0:N0} files' -f $total }
        $filesRect = New-Object Drawing.Rectangle([Math]::Max($script:filesX, $e.Bounds.Right + 8), $e.Bounds.Y, $script:filesW, $e.Bounds.Height)
        $filesFlags = [Windows.Forms.TextFormatFlags]'VerticalCenter, Right, NoPrefix, SingleLine'
        $filesColor = if ($studyBrush) { [Drawing.Color]::FromArgb(60, 60, 60) } else { [Drawing.Color]::DimGray }
        [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $filesText, $tree.Font, $filesRect, $filesColor, $filesFlags)

        $comment = $e.Node.Tag.Comment
        if ($comment) {
            $x = [Math]::Max($script:commentX, $filesRect.Right + 24)
            # Warnings ("also in", "duplicated") are drawn in red: a red mark first, so it is visible
            # even when the text does not fit, and the warning text after the comment
            $warnAt = $comment.IndexOf([char]0x26A0)
            $main = if ($warnAt -gt 0) { $comment.Substring(0, $warnAt).TrimEnd() } else { $comment }
            $red = [Drawing.Color]::FromArgb(190, 0, 0)
            if ($warnAt -gt 0) {
                $markRect = New-Object Drawing.Rectangle(($x - 20), $e.Bounds.Y, 20, $e.Bounds.Height)
                [Windows.Forms.TextRenderer]::DrawText($e.Graphics, [string][char]0x26A0, $tree.Font, $markRect, $red, $flags)
            }
            $rect = New-Object Drawing.Rectangle($x, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $x), $e.Bounds.Height)
            $color = if ($studyBrush) { [Drawing.Color]::Black }
                     elseif ($comment -like 'Counting*' -or $comment -like '(0 DICOMs*') { [Drawing.Color]::Gray }
                     else { [Drawing.Color]::FromArgb(0, 95, 170) }
            [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $main, $tree.Font, $rect, $color, $flags -bor [Windows.Forms.TextFormatFlags]::EndEllipsis)
            if ($warnAt -gt 0) {
                $mainWidth = [Windows.Forms.TextRenderer]::MeasureText($e.Graphics, $main, $tree.Font, $rect.Size, $flags).Width
                $wx = $x + $mainWidth + 6
                $warnRect = New-Object Drawing.Rectangle($wx, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $wx), $e.Bounds.Height)
                [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $comment.Substring($warnAt), $tree.Font, $warnRect, $red, $flags -bor [Windows.Forms.TextFormatFlags]::EndEllipsis)
            }
        }
    })
    $tree.add_NodeMouseDoubleClick({
        param($sender, $e)
        Start-Process explorer.exe ('"{0}"' -f $e.Node.Tag.Path)
    })
    $form.add_KeyDown({
        param($sender, $e)
        if ($e.KeyCode -eq 'F5') { Update-Tree; $e.Handled = $true }
    })
    $form.add_Shown({ $form.Activate() })

    # Live updates: a count writes desktop.ini folder by folder, and folders can be created,
    # deleted or renamed meanwhile. The watchers only collect what changed (SynchronizingObject
    # delivers their events on the window thread, as PowerShell requires); the timer applies it
    # in batches, since each comment is written in several steps, and repaints only those rows.
    $script:dirty = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:dirtyParents = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:dirtyFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:needsRebuild = $false

    # Files created, deleted or renamed: that folder is recounted
    $fileWatcher = New-Object IO.FileSystemWatcher($root)
    $fileWatcher.IncludeSubdirectories = $true
    $fileWatcher.NotifyFilter = [IO.NotifyFilters]::FileName
    $fileWatcher.InternalBufferSize = 65536
    $fileWatcher.SynchronizingObject = $form
    $onFileChange = {
        param($sender, $e)
        if ([IO.Path]::GetFileName($e.FullPath) -ne 'desktop.ini') {
            [void]$script:dirtyFiles.Add([IO.Path]::GetDirectoryName($e.FullPath))
        }
    }
    $fileWatcher.add_Created($onFileChange)
    $fileWatcher.add_Deleted($onFileChange)
    $fileWatcher.add_Renamed($onFileChange)
    $fileWatcher.add_Error({ $script:needsRebuild = $true })

    $dirWatcher = New-Object IO.FileSystemWatcher($root)
    $dirWatcher.IncludeSubdirectories = $true
    $dirWatcher.NotifyFilter = [IO.NotifyFilters]::DirectoryName
    $dirWatcher.InternalBufferSize = 65536
    $dirWatcher.SynchronizingObject = $form
    $onDirChange = {
        param($sender, $e)
        [void]$script:dirtyParents.Add([IO.Path]::GetDirectoryName($e.FullPath))
        if ($e -is [IO.RenamedEventArgs]) { [void]$script:dirtyParents.Add([IO.Path]::GetDirectoryName($e.OldFullPath)) }
    }
    $dirWatcher.add_Created($onDirChange)
    $dirWatcher.add_Deleted($onDirChange)
    $dirWatcher.add_Renamed($onDirChange)
    # Buffer overflow (too many changes at once): rebuild the whole tree
    $dirWatcher.add_Error({ $script:needsRebuild = $true })

    $watcher = New-Object IO.FileSystemWatcher($root, 'desktop.ini')
    $watcher.IncludeSubdirectories = $true
    $watcher.NotifyFilter = [IO.NotifyFilters]'FileName, LastWrite, Size'
    $watcher.InternalBufferSize = 65536
    $watcher.SynchronizingObject = $form
    $onChange = { param($sender, $e) [void]$script:dirty.Add([IO.Path]::GetDirectoryName($e.FullPath)) }
    $watcher.add_Changed($onChange)
    $watcher.add_Created($onChange)
    $watcher.add_Deleted($onChange)
    $watcher.add_Renamed($onChange)
    # Buffer overflow (too many changes at once): re-read every folder
    $watcher.add_Error({ foreach ($p in @($script:nodeByPath.Keys)) { [void]$script:dirty.Add($p) } })

    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 300
    $timer.add_Tick({
        if ($script:problemsCheckAt -and (Get-Date) -gt $script:problemsCheckAt) {
            $script:problemsCheckAt = $null
            Update-ProblemsButton
        }
        if ($script:needsRebuild) {
            $script:needsRebuild = $false
            $script:dirtyParents.Clear()
            $script:dirtyFiles.Clear()
            Update-Tree
        }
        if ($script:dirtyParents.Count) {
            # Parents first, so a new folder inside another new folder already has its place
            $parents = @($script:dirtyParents) | Sort-Object Length
            $script:dirtyParents.Clear()
            $tree.BeginUpdate()
            foreach ($p in $parents) {
                $node = $script:nodeByPath[$p]
                if ($node) { Sync-Children $node }
            }
            $tree.EndUpdate()
            Update-StudyColors
            Update-Status
        }
        if ($script:dirtyFiles.Count) {
            $dirs = @($script:dirtyFiles)
            $script:dirtyFiles.Clear()
            foreach ($d in $dirs) {
                $node = $script:nodeByPath[$d]
                if (-not $node) { continue }     # new folder: counted when its node is added
                $direct = Get-DirectFileCount $d
                $delta = $direct - $node.Tag.DirectFiles
                $node.Tag.DirectFiles = $direct
                Add-FileDelta $node $delta
            }
            Update-Status
        }
        if ($script:dirty.Count -eq 0) { return }
        $changed = @($script:dirty)
        $script:dirty.Clear()
        foreach ($dir in $changed) {
            $node = $script:nodeByPath[$dir]
            if (-not $node) { continue }
            $comment = Get-FolderComment $dir
            if ($comment -ne $node.Tag.Comment) {
                $node.Tag.Comment = $comment
                $node.Tag.StudyUid = Get-StudyUid $comment
                $node.ToolTipText = $comment
                Repaint-Row $node
            }
            Set-NodeIcon $node
        }
        Update-StudyColors
        Update-Status
        # The root is the last folder written by a count; its report is saved right after,
        # so the Problems button is refreshed a moment later
        if ($changed -contains $root) { $script:problemsCheckAt = (Get-Date).AddSeconds(2) }
    })

    Update-Tree
    $watcher.EnableRaisingEvents = $true
    $dirWatcher.EnableRaisingEvents = $true
    $fileWatcher.EnableRaisingEvents = $true
    $timer.Start()
    [void]$form.ShowDialog()
    $timer.Stop()
    $watcher.Dispose()
    $dirWatcher.Dispose()
    $fileWatcher.Dispose()
}

if ($RestoreQuarantine) {
    $r = Restore-Quarantine $RestoreQuarantine.TrimEnd('\')
    Write-Host ("Restored {0:N0} files ({1:N0} skipped: already back or missing)" -f $r.Restored, $r.Skipped)
    return
}

if ($Watch) {
    $isFirst = $false
    $mutex = New-Object Threading.Mutex($true, 'Local\DicomStudyCountWatcher', [ref]$isFirst)
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
                        [void][DicomStudyCount.ExplorerColumns]::ShowCommentColumn($window)
                        # Folder icons lost (deleted cache, another PC): rebuilt from their names
                        try { [DicomStudyCount.FolderIcons]::RepairChildren($path) } catch { }
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
    # Long form: the same folder through an 8.3 short name ("ERNEST~1") must be the same location
    $root = [DicomStudyCount.Paths]::Long((Resolve-Path -LiteralPath $Path).ProviderPath)

    if ($ShowDuplicates) {
        $copies = [DicomStudyCount.Duplicates]::Of($copiesDir, $root) | ForEach-Object { $_ -replace ' \(differs\)$', '' }
        $shown = if ($copies) { [DicomStudyCount.Duplicates]::Show([string[]]$copies) } else { 0 }
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
        Show-DicomTree $root
        return
    }

    if ($ToggleColumns) {
        Switch-DicomColumns $root
        return
    }

    if ($Unblock) {
        $r = [DicomStudyCount.Unblocker]::Run($root, [Environment]::ProcessorCount)
        Write-Host ("Unblocked {0:N0} DICOM files in {1}" -f $r[0], $root)
        if ($r[1]) { Write-Host ("Left marked: {0:N0} files that are not DICOM" -f $r[1]) }
        if ($r[2]) { Write-Host ("Could not unblock {0:N0} files" -f $r[2]) -ForegroundColor Yellow }
        return
    }

    if ($Clean -or $ClearComments) {
        Stop-CountsOn $root
        # Comments, icons, folder type and the attributes added to the folders
        $cleared = 0
        foreach ($ini in Get-DesktopIniFiles $root) { if ([DicomStudyCount.FolderComments]::Clear($ini.DirectoryName)) { $cleared++ } }
        # The count also added the column to the parent folder: removed there too, unless other
        # folders in it still have comments
        $parent = Split-Path -Path $root -Parent
        $parentToo = $parent -and -not (Test-HasDicomComments $parent)
        Hide-CommentColumn $root -Parent:$parentToo
        $views = [DicomStudyCount.ViewBags]::RemoveCommentColumn($root, $true)
        if ($parentToo) { $views += [DicomStudyCount.ViewBags]::RemoveCommentColumn($parent, $false) }
        # Copies in other locations will no longer point here, and the reports of the counts made
        # inside the folder are deleted
        $forgotten = [DicomStudyCount.CountedData]::ForgetIndex($indexDir, $root)
        [void][DicomStudyCount.CountedData]::ForgetCopies($copiesDir, $root)
        $reports = 0
        foreach ($file in Get-ChildItem -LiteralPath $reportDir -Filter '*.json' -ErrorAction SilentlyContinue) {
            try { $reportRoot = (Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json).Root } catch { continue }
            if ($reportRoot -and [DicomStudyCount.Paths]::IsUnder($reportRoot, $root)) {
                Remove-Item -LiteralPath $file.FullName -Force
                $reports++
            }
        }
        Write-Host "Cleaned $root"
        Write-Host ("  Folders with comments and icons removed: {0:N0}" -f $cleared)
        Write-Host ("  Saved Explorer views without the Comments column: {0:N0}" -f $views)
        Write-Host ("  Files forgotten in the index of counted locations: {0:N0}" -f $forgotten)
        Write-Host ("  Problems reports deleted: {0:N0}" -f $reports)
        return
    }

    if ($WriteComments) { Show-CommentColumn $root }

    $progress = New-Object DicomStudyCount.ScanProgress
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $task = [DicomStudyCount.Scanner]::StartScan($root, [Environment]::ProcessorCount, $progress, [bool]$WriteComments, $indexDir)
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
