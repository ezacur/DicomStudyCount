<#
.SYNOPSIS
    Makes the zip to download and use the suite: only what it runs with (explorer4dicoms.ps1,
    Install.cmd, Uninstall.cmd, bin\, lib\, the scripts of tools\ that -Install calls and the one of the
    Lister plugin, README.md) and the tutorial (tutorial.html, one file), in a folder explorer4dicoms\.
    Unzipped, Install.cmd installs it.
.EXAMPLE
    .\tools\New-Release.ps1                         # release\explorer4dicoms.zip
    .\tools\New-Release.ps1 -Out C:\temp\explorer4dicoms.zip
#>
param([string]$Out)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
# (release\ is not in the repository: the zip goes to the GitHub release)
if (-not $Out) { $Out = Join-Path $root 'release\explorer4dicoms.zip' }
$files = @('explorer4dicoms.ps1', 'Install.cmd', 'Uninstall.cmd', 'README.md', 'tutorial.html',
           'tools\Register-DicomIcons.ps1', 'tools\Register-DicomProperties.ps1', 'tools\Remove-AtReboot.ps1',
           'tools\PendingDeletes.ps1', 'tools\New-ListerPlugin.ps1')
$files += Get-ChildItem (Join-Path $root 'bin'), (Join-Path $root 'lib') -File | ForEach-Object { $_.FullName.Substring($root.Length + 1) }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) | Out-Null
$zip = [IO.Compression.ZipFile]::Open($Out, 'Create')
try {
    # (names with '/', as zips have them: Windows PowerShell 5.1 would write '\')
    foreach ($f in $files) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $root $f), 'explorer4dicoms/' + ($f -replace '\\', '/'))
    }
}
finally { $zip.Dispose() }
Write-Host ("Written $Out ({0:N0} files, {1:N1} MB)" -f $files.Count, ((Get-Item -LiteralPath $Out).Length / 1MB))
