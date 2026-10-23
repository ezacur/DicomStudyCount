<#
.SYNOPSIS
    Builds DicomIcons.dll, the Explorer icon handler for DICOM files (needs the Visual Studio C++
    build tools). Count-DicomStudies.ps1 -Install registers it when shellext\bin\DicomIcons.dll exists.
#>
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (C++ build tools) not found' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'The Visual Studio C++ build tools (x64) are not installed' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$src = Join-Path $PSScriptRoot 'DicomIcons.cpp'
$bin = Join-Path $PSScriptRoot 'bin'
$obj = Join-Path $bin 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
# Static runtime (/MT): the DLL runs inside Explorer and must not depend on a VC++ redistributable
$cl = 'cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /LD "{0}" /Fo"{1}/" /Fe"{2}"' -f $src, $obj, (Join-Path $bin 'DicomIcons.dll')
cmd /c "`"$vcvars`" >nul 2>nul && $cl 2>&1" | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
Write-Host "Built $(Join-Path $bin 'DicomIcons.dll')"
