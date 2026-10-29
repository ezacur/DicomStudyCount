<#
.SYNOPSIS
    Builds the two DLLs of bin\ from src\: only needed after changing the sources (the DLLs are in the
    repository, so a copy of the folder works without building anything).
      bin\Explorer4Dicoms.dll   src\Explorer4Dicoms\     C# for .NET Framework 4 (Windows PowerShell 5.1)
      bin\DicomShell.dll        src\*.cpp, src\*.h       C++ icons, columns and preview (x64), it needs
                                                         the Visual Studio C++ build tools, DCMTK,
                                                         OpenJPEG, libjxl and zlib (third_party),
                                                         built once into build\
.EXAMPLE
    .\tools\Build.ps1              # both
    .\tools\Build.ps1 -CSharp      # only the C# one
    .\tools\Build.ps1 -Dcmtk       # only DCMTK (again, even if it is built already)
    .\tools\Build.ps1 -OpenJpeg    # only OpenJPEG (again)
    .\tools\Build.ps1 -Zlib        # only zlib (again; and DCMTK after it)
    .\tools\Build.ps1 -Jxl         # only libjxl (again)
#>
param([switch]$CSharp, [switch]$Cpp, [switch]$Dcmtk, [switch]$OpenJpeg, [switch]$Zlib, [switch]$Jxl)
$ErrorActionPreference = 'Stop'
if (-not $CSharp -and -not $Cpp -and -not $Dcmtk -and -not $OpenJpeg -and -not $Zlib -and -not $Jxl) { $CSharp = $true; $Cpp = $true }
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

# DCMTK from third_party\dcmtk-*, as static libraries: a subset of its modules
# (builtin dictionary, no OpenSSL / PNG / TIFF / XML / iconv, wide file names) with zlib (deflated
# datasets) and oficonv (the character sets of DICOM text; its tables built in, no data files), and
# the static C runtime (/MT), the one of DicomShell.dll, which must not need a VC++ redistributable.
# Built again when the one there was built without zlib or oficonv, or from another version (then
# from scratch: the CMake cache of build\dcmtk names the folder of its sources).
$dcmtkSource = (Get-ChildItem (Join-Path $root 'third_party') -Directory -Filter 'dcmtk-*' | Select-Object -First 1).FullName
$dcmtkVersion = (Get-Content (Join-Path $dcmtkSource 'VERSION') -TotalCount 1).Trim()
$dcmtkConfig = Join-Path $dcmtkInstall 'include\dcmtk\config\osconfig.h'
$dcmtkHasZlib = (Test-Path $dcmtkConfig) -and (Select-String -Path $dcmtkConfig -Pattern '#define WITH_ZLIB' -Quiet) -and
    (Select-String -Path $dcmtkConfig -Pattern '#define DCMTK_ENABLE_CHARSET_CONVERSION DCMTK_CHARSET_CONVERSION_OFICONV' -SimpleMatch -Quiet)
$dcmtkSame = (Test-Path $dcmtkConfig) -and
    (Select-String -Path $dcmtkConfig -Pattern "#define PACKAGE_VERSION `"$dcmtkVersion`"" -SimpleMatch -Quiet)
if ($Dcmtk -or ($Cpp -and (-not (Test-Path (Join-Path $dcmtkInstall 'lib\dcmdata.lib')) -or -not $dcmtkHasZlib -or -not $dcmtkSame))) {
    $vs = Get-VisualStudio
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
    $source = $dcmtkSource
    $work = Join-Path $root 'build\dcmtk'
    if (-not $dcmtkSame) {
        foreach ($old in $work, $dcmtkInstall) { if (Test-Path $old) { Remove-Item -LiteralPath $old -Recurse -Force } }
    }
    # No -G: CMake takes the newest Visual Studio it finds (the one of its own installation)
    & $cmake -S $source -B $work -A x64 `
        '-DDCMTK_MODULES=ofstd;oflog;oficonv;dcmdata;dcmimgle;dcmimage;dcmjpeg;dcmjpls' `
        -DBUILD_SHARED_LIBS=OFF -DBUILD_APPS=OFF -DDCMTK_COMPILE_WIN32_MULTITHREADED_DLL=OFF `
        -DDCMTK_DEFAULT_DICT=builtin -DDCMTK_WIDE_CHAR_FILE_IO_FUNCTIONS=ON `
        -DDCMTK_WITH_OPENSSL=OFF -DDCMTK_WITH_PNG=OFF -DDCMTK_WITH_TIFF=OFF -DDCMTK_WITH_XML=OFF `
        -DDCMTK_WITH_ZLIB=ON "-DWITH_ZLIBINC=$zlibInstall" -DDCMTK_WITH_SNDFILE=OFF -DDCMTK_WITH_ICONV=OFF -DDCMTK_WITH_WRAP=OFF `
        -DDCMTK_WITH_DOXYGEN=OFF -DDCMTK_ENABLE_CHARSET_CONVERSION=oficonv -DDCMTK_ENABLE_BUILTIN_OFICONV_DATA=ON `
        "-DCMAKE_INSTALL_PREFIX=$dcmtkInstall"
    if ($LASTEXITCODE -ne 0) { throw 'DCMTK: CMake configuration failed' }
    & $cmake --build $work --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'DCMTK: build failed' }
    & $cmake --install $work --config Release
    if ($LASTEXITCODE -ne 0) { throw 'DCMTK: install failed' }
    Write-Host "Built DCMTK into $dcmtkInstall"
}

# OpenJPEG from third_party\openjpeg-*, for JPEG 2000: only its library (openjp2), static, /MT. Built
# again when the one there is of another version (then from scratch, as DCMTK)
$openJpegSource = (Get-ChildItem (Join-Path $root 'third_party') -Directory -Filter 'openjpeg-*' | Select-Object -First 1).FullName
$openJpegVersion = (Split-Path $openJpegSource -Leaf) -replace '^openjpeg-', ''
$openJpegConfig = Get-ChildItem (Join-Path $openJpegInstall 'include') -Recurse -Filter 'opj_config.h' -ErrorAction SilentlyContinue |
    Select-Object -First 1
$openJpegSame = $false
if ($openJpegConfig) {
    $v = Get-Content $openJpegConfig.FullName | ForEach-Object { if ($_ -match '#define OPJ_VERSION_(MAJOR|MINOR|BUILD) (\d+)') { $Matches[2] } }
    $openJpegSame = ($v -join '.') -eq $openJpegVersion
}
if ($OpenJpeg -or ($Cpp -and (-not (Test-Path (Join-Path $openJpegInstall 'lib\openjp2.lib')) -or -not $openJpegSame))) {
    $vs = Get-VisualStudio
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
    $work = Join-Path $root 'build\openjpeg'
    if (-not $openJpegSame) {
        foreach ($old in $work, $openJpegInstall) { if (Test-Path $old) { Remove-Item -LiteralPath $old -Recurse -Force } }
    }
    & $cmake -S $openJpegSource -B $work -A x64 `
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

# libjxl from third_party\libjxl-0.12.0, for JPEG XL: only its decoder (jxl_dec, with the Highway and
# Brotli it carries), static, /MT; into build\libjxl-install (its libraries and the headers, the
# generated ones too)
$jxlInstall = Join-Path $root 'build\libjxl-install'
if ($Jxl -or ($Cpp -and -not (Test-Path (Join-Path $jxlInstall 'lib\jxl_dec.lib')))) {
    $vs = Get-VisualStudio
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
    $source = (Get-ChildItem (Join-Path $root 'third_party') -Directory -Filter 'libjxl-*' | Select-Object -First 1).FullName
    $work = Join-Path $root 'build\libjxl'
    & $cmake -S $source -B $work -A x64 `
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF `
        -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_JNI=OFF `
        -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_BUNDLE_LIBPNG=OFF `
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
    if ($LASTEXITCODE -ne 0) { throw 'libjxl: CMake configuration failed' }
    & $cmake --build $work --config Release --target jxl_dec --parallel
    if ($LASTEXITCODE -ne 0) { throw 'libjxl: build failed' }
    New-Item -ItemType Directory -Force (Join-Path $jxlInstall 'lib'), (Join-Path $jxlInstall 'include\jxl') | Out-Null
    foreach ($lib in 'lib\Release\jxl_dec.lib', 'third_party\highway\Release\hwy.lib',
                     'third_party\brotli\Release\brotlidec.lib', 'third_party\brotli\Release\brotlicommon.lib') {
        Copy-Item (Join-Path $work $lib) (Join-Path $jxlInstall 'lib') -Force
    }
    Copy-Item (Join-Path $source 'lib\include\jxl\*.h'), (Join-Path $work 'lib\include\jxl\*.h') (Join-Path $jxlInstall 'include\jxl') -Force
    Write-Host "Built libjxl (decoder) into $jxlInstall"
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
        # (all the parts of src\Explorer4Dicoms\, compiled together)
        Add-Type -Path (Get-ChildItem (Join-Path $root 'src\Explorer4Dicoms') -Filter *.cs).FullName `
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
        # Media Foundation (video) loaded only when a video is decoded: Windows N has none without its Media Feature Pack
        # DCMTK, OpenJPEG, libjxl and zlib (static, from build\*-install) for the preview; their headers without warnings.
        $lib = Join-Path $dcmtkInstall 'lib'
        $libs = ('dcmimage', 'dcmimgle', 'dcmjpeg', 'ijg8', 'ijg12', 'ijg16', 'dcmjpls', 'dcmtkcharls', 'dcmdata', 'oficonv', 'oflog', 'ofstd' |
            ForEach-Object { '"{0}"' -f (Join-Path $lib "$_.lib") }) -join ' '
        $libs += ' "{0}" "{1}"' -f (Join-Path $openJpegInstall 'lib\openjp2.lib'), (Join-Path $zlibInstall 'lib\zlib_o.lib')
        $libs += ('jxl_dec', 'hwy', 'brotlidec', 'brotlicommon' | ForEach-Object { ' "{0}"' -f (Join-Path $jxlInstall "lib\$_.lib") }) -join ''
        $openJpegInclude = (Get-ChildItem (Join-Path $openJpegInstall 'include') -Directory | Select-Object -First 1).FullName
        # (all the parts of src\, at once: /MP)
        $sources = (Get-ChildItem (Join-Path $root 'src') -Filter *.cpp | ForEach-Object { '"{0}"' -f $_.FullName }) -join ' '
        $cl = ('/nologo /MP /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DOPJ_STATIC /DJXL_STATIC_DEFINE /external:W0 /external:I "{3}"' +
               ' /external:I "{5}" /external:I "{6}" /external:I "{7}" /LD {0} /Fo"{1}/" /Fe"{2}" /link {4} ws2_32.lib netapi32.lib advapi32.lib iphlpapi.lib delayimp.lib /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll') -f
            $sources, $work, (Join-Path $work 'DicomShell.dll'), (Join-Path $dcmtkInstall 'include'), $libs,
            $openJpegInclude, (Join-Path $zlibInstall 'include'), (Join-Path $jxlInstall 'include')
        # (in a response file: from a deep folder the line passes the 8191 characters of cmd)
        $rsp = Join-Path $work 'cl.rsp'
        [IO.File]::WriteAllText($rsp, $cl, [Text.Encoding]::Unicode)
        cmd /c "`"$vcvars`" >nul 2>nul && cl @`"$rsp`" 2>&1" | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
        Copy-Item -LiteralPath (Join-Path $work 'DicomShell.dll') -Destination (Join-Path $bin 'DicomShell.dll') -Force
        Write-Host "Built $(Join-Path $bin 'DicomShell.dll')"
    }
    finally { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host 'Run explorer4dicoms.ps1 -Install to register the new DLLs.'
