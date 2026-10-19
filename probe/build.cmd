@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /EHsc /W3 /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DNOMINMAX /Isrc /O2 /Fo:build\ /Fe:build\%1.exe probe\%1.cpp /link user32.lib gdi32.lib advapi32.lib shell32.lib shcore.lib comctl32.lib ole32.lib oleaut32.lib d2d1.lib shlwapi.lib
endlocal
