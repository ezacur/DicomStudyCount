<#
.SYNOPSIS
    Makes the zip to download and use the suite: only what it runs with (explorer4dicoms.ps1,
    Install.cmd, Uninstall.cmd, bin\, lib\, the scripts of tools\ that -Install calls and the one of the
    Lister plugin, README.md), in a folder explorer4dicoms\. Unzipped, Install.cmd installs it. With
    -Tutorial, also tutorial\ when it is there (it is not in the repository).
.EXAMPLE
    .\tools\New-Release.ps1                         # %TEMP%\explorer4dicoms.zip
    .\tools\New-Release.ps1 -Tutorial -Out C:\temp\explorer4dicoms.zip
#>
param([string]$Out = (Join-Path ([IO.Path]::GetTempPath()) 'explorer4dicoms.zip'), [switch]$Tutorial)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$files = @('explorer4dicoms.ps1', 'Install.cmd', 'Uninstall.cmd', 'README.md',
           'tools\Register-DicomIcons.ps1', 'tools\Register-DicomProperties.ps1', 'tools\New-ListerPlugin.ps1')
$files += Get-ChildItem (Join-Path $root 'bin'), (Join-Path $root 'lib') -File | ForEach-Object { $_.FullName.Substring($root.Length + 1) }
if ($Tutorial) {
    $dir = Join-Path $root 'tutorial'
    if (-not (Test-Path -LiteralPath $dir)) { throw "Not found: $dir" }
    $files += Get-ChildItem $dir -Recurse -File | ForEach-Object { $_.FullName.Substring($root.Length + 1) }
}
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
