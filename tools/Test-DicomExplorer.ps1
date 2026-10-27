<#
.SYNOPSIS
    Tests the DICOM explorer of bin\DicomShell.dll over folders of DICOM files, without its window:
    builds tools\ExplorerTest.cpp (with the sources of src\) and runs it. For each folder, the
    gathering, the tree, the checks, the middle thumbnail of every series, the report, and for a sample
    of orientations the floating thumbnail and the exports (PNG, animated GIF, NIfTI), each file written
    checked (readable, its frames; a NIfTI, its size from its header) and deleted. The folders are only
    read; the exports go to a temporary folder, removed at the end.
    Needs what tools\Build.ps1 -Cpp needs (the Visual Studio C++ build tools, DCMTK and OpenJPEG built
    into build\).
.EXAMPLE
    .\tools\Test-DicomExplorer.ps1 -Folders D:\DICOM\CT, D:\DICOM\MR             # 25 orientations each
    .\tools\Test-DicomExplorer.ps1 -Folders (Get-ChildItem D:\DICOM -Directory).FullName -Sample 10
    .\tools\Test-DicomExplorer.ps1 -Folders D:\DICOM -Missing     # the series with instance numbers missing
    powershell.exe -File .\tools\Test-DicomExplorer.ps1 D:\DICOM\CT D:\DICOM\MR -Sample 10   # (from outside:
                                                                   # the folders one after the other, no -Folders)
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    # (all the folders, also when they come loose: powershell.exe -File passes "a, b" as two arguments)
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)] [string[]]$Folders,
    [int]$Sample = 25,
    [string]$Log,
    [switch]$Missing
)
$ErrorActionPreference = 'Stop'
$Folders = @($Folders | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($f in $Folders) { if (-not (Test-Path -LiteralPath $f -PathType Container)) { throw "Not a folder: $f" } }
# The log is written by the test, never deleted here: it must be a file of its own
if ($Log -and ((Test-Path -LiteralPath $Log -PathType Container) -or $Log -notmatch '\.(log|txt)$')) { throw "-Log must be a .log or .txt file: $Log" }
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dcmtkInstall = Join-Path $root 'build\dcmtk-install'
$openJpegInstall = Join-Path $root 'build\openjpeg-install'
$jxlInstall = Join-Path $root 'build\libjxl-install'
if (-not (Test-Path (Join-Path $dcmtkInstall 'lib\dcmdata.lib')) -or -not (Test-Path (Join-Path $openJpegInstall 'lib\openjp2.lib')) -or
    -not (Test-Path (Join-Path $root 'build\zlib-install\lib\zlib_o.lib')) -or -not (Test-Path (Join-Path $jxlInstall 'lib\jxl_dec.lib'))) {
    throw 'DCMTK, OpenJPEG, libjxl and zlib are not built yet: run .\tools\Build.ps1 -Cpp first'
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) { throw 'The Visual Studio C++ build tools (x64) are needed' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$work = Join-Path ([IO.Path]::GetTempPath()) ('DicomExplorerTest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
if (-not $Log) { $Log = Join-Path ([IO.Path]::GetTempPath()) 'DicomExplorerTest.log' }
try {
    # The same options and libraries as bin\DicomShell.dll (tools\Build.ps1), an executable instead
    $lib = Join-Path $dcmtkInstall 'lib'
    $libs = ('dcmimage', 'dcmimgle', 'dcmjpeg', 'ijg8', 'ijg12', 'ijg16', 'dcmjpls', 'dcmtkcharls', 'dcmdata', 'oficonv', 'oflog', 'ofstd' |
        ForEach-Object { '"{0}"' -f (Join-Path $lib "$_.lib") }) -join ' '
    $zlibInstall = Join-Path $root 'build\zlib-install'
    $libs += ' "{0}" "{1}"' -f (Join-Path $openJpegInstall 'lib\openjp2.lib'), (Join-Path $zlibInstall 'lib\zlib_o.lib')
    $libs += ('jxl_dec', 'hwy', 'brotlidec', 'brotlicommon' | ForEach-Object { ' "{0}"' -f (Join-Path $jxlInstall "lib\$_.lib") }) -join ''
    $openJpegInclude = (Get-ChildItem (Join-Path $openJpegInstall 'include') -Directory | Select-Object -First 1).FullName
    $exe = Join-Path $work 'ExplorerTest.exe'
    $sources = ((@(Join-Path $root 'tools\ExplorerTest.cpp') + (Get-ChildItem (Join-Path $root 'src') -Filter *.cpp).FullName) |
        ForEach-Object { '"{0}"' -f $_ }) -join ' '
    $cl = ('cl /nologo /MP /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /DOPJ_STATIC /DJXL_STATIC_DEFINE /external:W0 /external:I "{0}" /external:I "{1}"' +
           ' /external:I "{6}" /external:I "{7}" {2} /Fo"{3}\\" /Fe"{4}" /link {5} ws2_32.lib netapi32.lib advapi32.lib iphlpapi.lib') -f
        (Join-Path $dcmtkInstall 'include'), $openJpegInclude, $sources, $work, $exe, $libs,
        (Join-Path $zlibInstall 'include'), (Join-Path $jxlInstall 'include')
    cmd /c "`"$vcvars`" >nul 2>nul && $cl 2>&1" | Where-Object { $_ -match 'error|warning' } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "The test did not build ($LASTEXITCODE)" }

    # (DCMTK writes its warnings about odd files to stderr: not wanted here, and in Windows PowerShell 5.1
    # each line of it would stop this script while 'Stop' is in force)
    $ErrorActionPreference = 'Continue'
    if ($Missing) { & $exe --missing $Log @Folders 2>$null | Out-Null }
    else { & $exe (Join-Path $work 'out') $Sample $Log @Folders 2>$null | Out-Null }
    $ErrorActionPreference = 'Stop'
    Get-Content -LiteralPath $Log -Encoding UTF8
    Write-Host "`nLog: $Log"
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
