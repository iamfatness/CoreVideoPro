@echo off
set "APP=%~dp0artifacts\desktop\win-unpacked\CoreVideo Pro.exe"
if not exist "%APP%" (
  echo Packaged app not found. Build it once with:
  echo   npm run pack:desktop
  pause
  exit /b 1
)
start "" "%APP%"