<#
.SYNOPSIS
    What is left of the suite (%ProgramFiles%\explorer4dicoms and the folder of its data): deleted now what
    nothing has in use, and the rest (a DLL Explorer or the search indexer still has loaded) by Windows at
    the next restart. Called by explorer4dicoms.ps1 -FullClean (it needs administrator rights); -Install
    takes the deletions at the restart back.
#>
param([Parameter(Mandatory = $true)][string]$UserData)
$ErrorActionPreference = 'Stop'
if (-not (New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator rights are needed (run it elevated)'
}
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'PendingDeletes.ps1')
$dirs = (Join-Path $env:ProgramFiles 'explorer4dicoms'), $UserData
foreach ($d in $dirs) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue } }
$n = Add-PendingDeletes -Under $dirs
if ($n) { Write-Host "Deleted at the next restart: $n files and folders still in use" }
else { Write-Host 'Nothing left of the suite' }
