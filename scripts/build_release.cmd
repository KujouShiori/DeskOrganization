@echo off
setlocal
set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "CMAKE=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

rem Release config for distribution: optimizations on, and the C runtime is
rem linked statically so the portable zip runs without the VC++ redistributable.
"%CMAKE%" -S "%~dp0.." -B "%~dp0..\out\release" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" || exit /b 1

"%CMAKE%" --build "%~dp0..\out\release" || exit /b 1
echo Built: %~dp0..\out\release\DeskOrganization.exe
endlocal
