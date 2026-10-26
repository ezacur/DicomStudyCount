<#
.SYNOPSIS
    Builds the two DLLs of bin\ from src\: only needed after changing the sources (the DLLs are in the
    repository, so a copy of the folder works without building anything).
      bin\Explorer4Dicoms.dll   src\Explorer4Dicoms.cs   C# for .NET Framework 4 (Windows PowerShell 5.1)
      bin\DicomShell.dll        src\DicomShell.cpp       C++ icons, columns and preview (x64), it needs
                                                         the Visual Studio C++ build tools, DCMTK,
                                                         OpenJPEG and zlib (third_party), built once
                                                         into build\
.EXAMPLE
    .\tools\Build.ps1              # both
    .\tools\Build.ps1 -CSharp      # only the C# one
    .\tools\Build.ps1 -Dcmtk       # only DCMTK (again, even if it is built already)
    .\tools\Build.ps1 -OpenJpeg    # only OpenJPEG (again)
    .\tools\Build.ps1 -Zlib        # only zlib (again; and DCMTK after it)
#>
param([switch]$CSharp, [switch]$Cpp, [switch]$Dcmtk, [switch]$OpenJpeg, [switch]$Zlib)
$ErrorActionPreference = 'Stop'
if (-not $CSharp -and -not $Cpp -and -not $Dcmtk -and -not $OpenJpeg -and -not $Zlib) { $CSharp = $true; $Cpp = $true }
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bin = Join-Path $root 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
$dcmtkInstall = Join-Path $root 'build\dcmtk-install'
$openJpegInstall = Join-Path $root 'build\openjpeg-install'

function Get-VisualStudio {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { throw 'The Visual Studio C++ build tools (x64) are needed for bin\DicomShell.dll' }
    $vs
}

# zlib from third_party\zlib-1.3.1, static, /MT: for DCMTK (deflated datasets) and for the deflated
# image frames DicomShell decodes itself. Installed with the names DCMTK looks for (zlib_o.lib)
$zlibInstall = Join-Path $root 'build\zlib-install'
if ($Zlib -or ($Cpp -and -not (Test-Path (Join-Path $zlibInstall 'lib\zlib_o.lib')))) {
    $vs = Get-VisualStudio
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
    $work = Join-Path $root 'build\zlib'
    # (its CMakeLists asks for CMake 2.4: CMAKE_POLICY_VERSION_MINIMUM lets a CMake 4 take it)
    & $cmake -S (Join-Path $root 'third_party\zlib-1.3.1') -B $work -A x64 `
        -DZLIB_BUILD_EXAMPLES=OFF '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' `
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' `
        "-DCMAKE_INSTALL_PREFIX=$zlibInstall"
    if ($LASTEXITCODE -ne 0) { throw 'zlib: CMake configuration failed' }
    & $cmake --build $work --config Release --target zlibstatic --parallel
    if ($LASTEXITCODE -ne 0) { throw 'zlib: build failed' }
    New-Item -ItemType Directory -Force (Join-Path $zlibInstall 'include'), (Join-Path $zlibInstall 'lib') | Out-Null
    Copy-Item (Join-Path $root 'third_party\zlib-1.3.1\zlib.h'), (Join-Path $work 'zconf.h') (Join-Path $zlibInstall 'include') -Force
    foreach ($name in 'zlib_o.lib', 'zlib_d.lib') { Copy-Item (Join-Path $work 'Release\zlibstatic.lib') (Join-Path $zlibInstall "lib\$name") -Force }
    Write-Host "Built zlib into $zlibInstall"
    $Dcmtk = $true                                  # (DCMTK again, with it)
}

# DCMTK from third_party\dcmtk-3.7.0, as static libraries: the subset and options of I3Dfileformat
# (builtin dictionary, no OpenSSL / PNG / TIFF / XML / iconv, wide file names) with zlib (deflated
# datasets), and the static C runtime (/MT), the one of DicomShell.dll, which must not need a VC++
# redistributable. Built again when the one there was built without zlib.
$dcmtkConfig = Join-Path $dcmtkInstall 'include\dcmtk\config\osconfig.h'
$dcmtkHasZlib = (Test-Path $dcmtkConfig) -and (Select-String -Path $dcmtkConfig -Pattern '#define WITH_ZLIB' -Quiet)
if ($Dcmtk -or ($Cpp -and (-not (Test-Path (Join-Path $dcmtkInstall 'lib\dcmdata.lib')) -or -not $dcmtkHasZlib))) {
    $vs = Get-VisualStudio
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
    $source = Join-Path $root 'third_party\dcmtk-3.7.0'
    $work = Join-Path $root 'build\dcmtk'
    # No -G: CMake takes the newest Visual Studio it finds (the one of its own installation)
    & $cmake -S $source -B $work -A x64 `
        '-DDCMTK_MODULES=ofstd;oflog;dcmdata;dcmimgle;dcmimage;dcmjpeg;dcmjpls' `
        -DBUILD_SHARED_LIBS=OFF -DBUILD_APPS=OFF -DDCMTK_COMPILE_WIN32_MULTITHREADED_DLL=OFF `
        -DDCMTK_DEFAULT_DICT=builtin -DDCMTK_WIDE_CHAR_FILE_IO_FUNCTIONS=ON `
        -DDCMTK_WITH_OPENSSL=OFF -DDCMTK_WITH_PNG=OFF -DDCMTK_WITH_TIFF=OFF -DDCMTK_WITH_XML=OFF `
        -DDCMTK_WITH_ZLIB=ON "-DWITH_ZLIBINC=$zlibInstall" -DDCMTK_WITH_SNDFILE=OFF -DDCMTK_WITH_ICONV=OFF -DDCMTK_WITH_WRAP=OFF `
        -DDCMTK_WITH_DOXYGEN=OFF '-DDCMTK_ENABLE_CHARSET_CONVERSION=<disabled>' `
        "-DCMAKE_INSTALL_PREFIX=$dcmtkInstall"
    if ($LASTEXITCODE -ne 0) { throw 'DCMTK: CMake configuration failed' }
    & $cmake --build $work --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'DCMTK: build failed' }
    & $cmake --install $work --config Release
    if ($LASTEXITCODE -ne 0) { throw 'DCMTK: install failed' }
    Write-Host "Built DCMTK into $dcmtkInstall"
}

# OpenJPEG from third_party\openjpeg-2.5.3, for JPEG 2000: only its library (openjp2), static, /MT
if ($OpenJpeg -or ($Cpp -and -not (Test-Path (Join-Path $openJpegInstall 'lib\openjp2.lib')))) {
    $vs = Get-VisualStudio
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
    $work = Join-Path $root 'build\openjpeg'
    & $cmake -S (Join-Path $root 'third_party\openjpeg-2.5.3') -B $work -A x64 `
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF -DBUILD_DOC=OFF `
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' `
        "-DCMAKE_INSTALL_PREFIX=$openJpegInstall"
    if ($LASTEXITCODE -ne 0) { throw 'OpenJPEG: CMake configuration failed' }
    & $cmake --build $work --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'OpenJPEG: build failed' }
    & $cmake --install $work --config Release
    if ($LASTEXITCODE -ne 0) { throw 'OpenJPEG: install failed' }
    Write-Host "Built OpenJPEG into $openJpegInstall"
}

if ($CSharp) {
    # The C# compiler of Windows PowerShell 5.1 builds for .NET Framework, the one the script runs on;
    # PowerShell 7 would build for .NET Core
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path -CSharp
        if ($LASTEXITCODE -ne 0) { throw 'C# build failed' }
    }
    else {
        $target = Join-Path $bin 'Explorer4Dicoms.dll'
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N') + '.dll')
        Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $root 'src\Explorer4Dicoms.cs'))) -Language CSharp `
            -ReferencedAssemblies System.Drawing -OutputAssembly $tmp -OutputType Library
        Move-Item -LiteralPath $tmp -Destination $target -Force
        Write-Host "Built $target"
    }
}

if ($Cpp) {
    $vcvars = Join-Path (Get-VisualStudio) 'VC\Auxiliary\Build\vcvars64.bat'
    $work = Join-Path ([IO.Path]::GetTempPath()) ('DicomShell-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        # Static runtime (/MT): it runs inside Explorer and must not depend on a VC++ redistributable.
        # DCMTK, OpenJPEG and zlib (static, from build\*-install) for the preview; their headers without warnings.
        $lib = Join-Path $dcmtkInstall 'lib'
        $libs = ('dcmimage', 'dcmimgle', 'dcmjpeg', 'ijg8', 'ijg12', 'ijg16', 'dcmjpls', 'dcmtkcharls', 'dcmdata', 'oflog', 'ofstd' |
            ForEach-Object { '"{0}"' -f (Join-Path $lib "$_.lib") }) -join ' '
        $libs += ' "{0}" "{1}"' -f (Join-Path $openJpegInstall 'lib\openjp2.lib'), (Join-Path $zlibInstall 'lib\zlib_o.lib')
        $openJpegInclude = (Get-ChildItem (Join-Path $openJpegInstall 'include') -Directory | Select-Object -First 1).FullName
        $cl = ('cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DOPJ_STATIC /external:W0 /external:I "{3}"' +
               ' /external:I "{5}" /external:I "{6}" /LD "{0}" /Fo"{1}/" /Fe"{2}" /link {4} ws2_32.lib netapi32.lib advapi32.lib iphlpapi.lib') -f
            (Join-Path $root 'src\DicomShell.cpp'), $work, (Join-Path $work 'DicomShell.dll'), (Join-Path $dcmtkInstall 'include'), $libs,
            $openJpegInclude, (Join-Path $zlibInstall 'include')
        cmd /c "`"$vcvars`" >nul 2>nul && $cl 2>&1" | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
        Copy-Item -LiteralPath (Join-Path $work 'DicomShell.dll') -Destination (Join-Path $bin 'DicomShell.dll') -Force
        Write-Host "Built $(Join-Path $bin 'DicomShell.dll')"
    }
    finally { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host 'Run explorer4dicoms.ps1 -Install to register the new DLLs.'
