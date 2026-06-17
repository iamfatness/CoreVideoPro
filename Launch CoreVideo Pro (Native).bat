@echo off
cd /d "%~dp0"
set "PACKED=%~dp0artifacts\native\win-unpacked\CoreVideo Pro.cmd"
if exist "%PACKED%" (
  call "%PACKED%"
  exit /b %ERRORLEVEL%
)
powershell -ExecutionPolicy Bypass -File scripts\launch-native.ps1