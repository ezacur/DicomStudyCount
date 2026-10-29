<#
.SYNOPSIS
    Makes the zip to download and use the suite: only what it runs with (explorer4dicoms.ps1,
    Install.cmd, Uninstall.cmd, bin\, lib\, the scripts of tools\ that -Install calls and the one of the
    Lister plugin, README.md), in a folder explorer4dicoms\. Unzipped, Install.cmd installs it. With
    -Tutorial, also tutorial.html: tutorial\index.html with its images and videos inside it (data: URIs),
    one file that needs nothing else (tutorial\ is not in the repository; its screenshots must show public
    data only).
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
$single = $null
if ($Tutorial) {
    $dir = Join-Path $root 'tutorial'
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'index.html'))) { throw "Not found: $dir\index.html" }
    # Every src="img/..." or src="video/..." as the file itself, in base64
    $types = @{ '.png' = 'image/png'; '.jpg' = 'image/jpeg'; '.gif' = 'image/gif'; '.svg' = 'image/svg+xml'; '.mp4' = 'video/mp4'; '.webm' = 'video/webm' }
    $html = [IO.File]::ReadAllText((Join-Path $dir 'index.html'), [Text.Encoding]::UTF8)
    $html = [regex]::Replace($html, 'src="((?:img|video)/[^"]+)"', {
        param($m)
        $path = Join-Path $dir ($m.Groups[1].Value -replace '/', '\')
        $type = $types[[IO.Path]::GetExtension($path).ToLowerInvariant()]
        if (-not $type) { throw "No media type for $path" }
        'src="data:' + $type + ';base64,' + [Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) + '"'
    })
    if ($html -match 'src="(?!data:)[^"]') { throw 'The tutorial still points to a file of its own' }
    $single = Join-Path ([IO.Path]::GetTempPath()) ('explorer4dicoms-tutorial-' + [Guid]::NewGuid().ToString('N') + '.html')
    [IO.File]::WriteAllText($single, $html, (New-Object Text.UTF8Encoding $false))
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
    if ($single) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $single, 'explorer4dicoms/tutorial.html'); $files += 'tutorial.html' }
}
finally {
    $zip.Dispose()
    if ($single) { Remove-Item -LiteralPath $single -Force -ErrorAction SilentlyContinue }
}
Write-Host ("Written $Out ({0:N0} files, {1:N1} MB)" -f $files.Count, ((Get-Item -LiteralPath $Out).Length / 1MB))
