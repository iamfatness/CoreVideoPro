@echo off
cd /d "%~dp0"
set "APP=%~dp0artifacts\native\win-unpacked\CoreVideo Pro.exe"
if exist "%APP%" (
  start "" "%APP%"
  exit /b 0
)
echo Packaged native shell not found. Building and launching from source...
powershell -ExecutionPolicy Bypass -File scripts\launch-native.ps1