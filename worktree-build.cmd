@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64
if errorlevel 1 exit /b 1
if "%1"=="configure" (
  cmake -S native -B native\build-dev -DCMAKE_BUILD_TYPE=Release
  exit /b %errorlevel%
)
cmake --build native\build-dev --config Release --target corevideo-native corevideo-native-tests
exit /b %errorlevel%
