@echo off
setlocal
set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "CMAKE=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if "%1"=="clean" rmdir /s /q "%~dp0..\out"

"%CMAKE%" -S "%~dp0.." -B "%~dp0..\out\build" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" || exit /b 1

"%CMAKE%" --build "%~dp0..\out\build" || exit /b 1
echo Built: %~dp0..\out\build\DeskOrganization.exe
endlocal
