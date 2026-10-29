@echo off
rem explorer4dicoms: installs it from this folder (unzipped, or cloned) with a double click.
rem The folder stays where it is: the context menu points to it, so keep it in a permanent place
rem (run this again if you move it). It removes the "downloaded from the Internet" mark from the
rem files of the folder first, since Windows would not run them from a zip otherwise.
set "E4D=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath $env:E4D -Recurse -File | Unblock-File"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0explorer4dicoms.ps1" -Install
echo.
pause
