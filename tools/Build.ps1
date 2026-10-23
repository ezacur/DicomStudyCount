<#
.SYNOPSIS
    Builds the two DLLs of bin\ from src\: only needed after changing the sources (the DLLs are in the
    repository, so a copy of the folder works without building anything).
      bin\DicomStudyCount.dll   src\DicomStudyCount.cs   C# for .NET Framework 4 (Windows PowerShell 5.1)
      bin\DicomShell.dll        src\DicomShell.cpp       C++ icon and property handler (x64), it needs
                                                         the Visual Studio C++ build tools
.EXAMPLE
    .\tools\Build.ps1              # both
    .\tools\Build.ps1 -CSharp      # only the C# one
#>
param([switch]$CSharp, [switch]$Cpp)
$ErrorActionPreference = 'Stop'
if (-not $CSharp -and -not $Cpp) { $CSharp = $true; $Cpp = $true }
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bin = Join-Path $root 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null

if ($CSharp) {
    # The C# compiler of Windows PowerShell 5.1 builds for .NET Framework, the one the script runs on;
    # PowerShell 7 would build for .NET Core
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path -CSharp
        if ($LASTEXITCODE -ne 0) { throw 'C# build failed' }
    }
    else {
        $target = Join-Path $bin 'DicomStudyCount.dll'
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N') + '.dll')
        Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $root 'src\DicomStudyCount.cs'))) -Language CSharp `
            -ReferencedAssemblies System.Drawing -OutputAssembly $tmp -OutputType Library
        Move-Item -LiteralPath $tmp -Destination $target -Force
        Write-Host "Built $target"
    }
}

if ($Cpp) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { throw 'The Visual Studio C++ build tools (x64) are needed for bin\DicomShell.dll' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $work = Join-Path ([IO.Path]::GetTempPath()) ('DicomShell-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        # Static runtime (/MT): it runs inside Explorer and must not depend on a VC++ redistributable
        $cl = 'cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /LD "{0}" /Fo"{1}/" /Fe"{2}"' -f
            (Join-Path $root 'src\DicomShell.cpp'), $work, (Join-Path $work 'DicomShell.dll')
        cmd /c "`"$vcvars`" >nul 2>nul && $cl 2>&1" | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
        Copy-Item -LiteralPath (Join-Path $work 'DicomShell.dll') -Destination (Join-Path $bin 'DicomShell.dll') -Force
        Write-Host "Built $(Join-Path $bin 'DicomShell.dll')"
    }
    finally { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host 'Run Count-DicomStudies.ps1 -Install to register the new DLLs.'
