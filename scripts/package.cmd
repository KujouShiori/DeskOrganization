@echo off
setlocal
rem Builds the Release exe, then the portable zip and the installer, into out\dist\.
rem Needs the VS 2022 C++ workload and Inno Setup 6.
rem The release version lives in CMakeLists.txt (DESKORG_VERSION). The configure step
rem writes it into the build dir as version.txt and it is read back here, so the
rem version stamped in the exe and the artifact names can never disagree.
rem Batch files here stay ASCII only: cmd parses them in the console codepage, and
rem non-ASCII bytes inside a rem line break parsing.

set "NAME=DeskOrganization"
set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
set "DIST=%~dp0..\out\dist"
set "REL=%~dp0..\out\release"

call "%~dp0build_release.cmd" || exit /b 1
if not exist "%REL%\%NAME%.exe" exit /b 1
if not exist "%REL%\version.txt" exit /b 1
set /p VERSION=<"%REL%\version.txt"
if not exist "%ISCC%" goto :no_inno

rd /s /q "%DIST%" 2>nul
mkdir "%DIST%" || exit /b 1

rem The zip keeps one top level folder, so extracting it does not scatter files.
mkdir "%DIST%\portable\%NAME%" || exit /b 1
copy /y "%REL%\%NAME%.exe"  "%DIST%\portable\%NAME%\" || exit /b 1
copy /y "%~dp0..\README.md" "%DIST%\portable\%NAME%\" || exit /b 1
copy /y "%~dp0..\LICENSE"   "%DIST%\portable\%NAME%\" || exit /b 1
rem bsdtar, spelled out because a GNU tar earlier on PATH cannot read Windows paths.
"%SystemRoot%\System32\tar.exe" -a -cf "%DIST%\%NAME%-%VERSION%-win-x64-portable.zip" -C "%DIST%\portable" "%NAME%" || exit /b 1
rd /s /q "%DIST%\portable" 2>nul

"%ISCC%" /DAppVersion=%VERSION% /O"%DIST%" "%~dp0installer.iss" || exit /b 1

echo.
echo Release artifacts in %DIST%
dir /b "%DIST%\%NAME%-%VERSION%-*"
endlocal
exit /b 0

:no_inno
echo Inno Setup was not found where ISCC points. Install Inno Setup 6, or edit that path.
exit /b 1
