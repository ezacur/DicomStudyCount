<#
.SYNOPSIS
    Makes the Lister plugin of Total Commander (64-bit) from bin\DicomShell.dll: a zip with it as
    explorer4dicoms.wlx64 and its pluginst.inf. Opened in Total Commander (Enter on it), Total Commander
    offers to install it, and does so itself (its plugins folder, its wincmd.ini).
.NOTES
    The plugin is the preview of Explorer's preview pane (the image, the series, the header) with the
    columns as the values of the file, without check boxes. Its keys: those of the preview; the rest go to
    the Lister (Esc, N, P...). A plugin Total Commander has loaded cannot be replaced while it runs: to
    update it, close Total Commander first.
.EXAMPLE
    .\tools\New-ListerPlugin.ps1                       # %TEMP%\explorer4dicoms-lister.zip
    .\tools\New-ListerPlugin.ps1 -Out C:\temp\lister.zip
#>
param([string]$Out = (Join-Path ([IO.Path]::GetTempPath()) 'explorer4dicoms-lister.zip'))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dll = Join-Path $root 'bin\DicomShell.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Not found: $dll (build it with tools\Build.ps1)" }
$work = Join-Path ([IO.Path]::GetTempPath()) ('explorer4dicoms-lister-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    Copy-Item -LiteralPath $dll -Destination (Join-Path $work 'explorer4dicoms.wlx64')
    # (file= names the 32-bit plugin; Total Commander 64-bit takes the same name with 64 after it)
    Set-Content -LiteralPath (Join-Path $work 'pluginst.inf') -Encoding Ascii -Value @(
        '[plugininstall]',
        'description=explorer4dicoms: DICOM images, their series and header (the preview of explorer4dicoms)',
        'type=wlx',
        'file=explorer4dicoms.wlx',
        'defaultdir=explorer4dicoms')
    if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
    Compress-Archive -Path (Join-Path $work '*') -DestinationPath $Out
    Write-Host "Made $Out"
    Write-Host 'In Total Commander: go to it and press Enter; it offers to install the plugin.'
}
finally { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
