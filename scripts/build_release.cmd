@echo off
setlocal
rem Release config for distribution: optimizations on, and the C runtime is
rem linked statically so the portable zip runs without the VC++ redistributable.
rem ASCII only, CRLF: cmd parses batch files in the console codepage.

rem vswhere ships with every Visual Studio 2017+ product, so this finds Build Tools,
rem Community/Professional/Enterprise and the CI runner image without hard-coding one
rem install path. The fallback keeps working on a machine without vswhere.
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto :no_vs

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

rem CMake and Ninja are on PATH on the CI image; a bare Build Tools install keeps them
rem under Common7, so prefer PATH and fall back to the bundled copies.
set "CMAKE=cmake"
set "NINJA=ninja"
where cmake >nul 2>&1 || set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
where ninja >nul 2>&1 || set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

"%CMAKE%" -S "%~dp0.." -B "%~dp0..\out\release" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" || exit /b 1

"%CMAKE%" --build "%~dp0..\out\release" || exit /b 1
echo Built: %~dp0..\out\release\DeskOrganization.exe
endlocal
exit /b 0

:no_vs
echo Could not find a Visual Studio with the C++ workload. Install it, or point VSROOT at it.
exit /b 1
