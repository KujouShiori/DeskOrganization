@echo off
setlocal
rem Regenerates the file icon candidates: out\iconpreview\*.png and out\appicon-*.ico.
rem The pixel algorithm lives in src\iconres.cpp, so the exe icon and the tray icon
rem can never drift apart. Run from the repository root.

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build

cl /nologo /EHsc /W3 /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DNOMINMAX /Isrc /O2 ^
  /Fo:build\ /Fe:build\make_icon.exe ^
  probe\make_icon.cpp src\iconres.cpp src\theme.cpp src\util.cpp ^
  /link user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib shlwapi.lib windowscodecs.lib || exit /b 1

build\make_icon.exe || exit /b 1
endlocal
