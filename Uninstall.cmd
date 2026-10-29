@echo off
rem explorer4dicoms: leaves the computer as before it (explorer4dicoms.ps1 -FullClean): every counted
rem folder cleaned (its comments and icons), the suite uninstalled, and its data, caches and settings
rem deleted. Never the DICOM files. This folder stays: delete it afterwards.
echo This removes explorer4dicoms from this computer: the comments and icons of every counted folder,
echo the context menu, the icons, preview and columns of the DICOM files, and all its data and settings.
echo The DICOM files are not touched.
choice /C YN /M "Go on"
if errorlevel 2 exit /b
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0explorer4dicoms.ps1" -FullClean
echo.
pause
