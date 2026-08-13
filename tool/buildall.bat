@echo off
rem Rebuild EVERY engine-embedding binary in one pass, so none can skew.
rem Written 2026-08-13 after a one-campaign-stale sqlite3.exe refused a
rem database the testfixture had just written (DOCKET #6): the repo has
rem FOUR binaries that embed the engine -- testfixture.exe, sqlite3.exe,
rem sqlite3.dll, procgen.exe -- and rebuilding some is the same trap as
rem rebuilding none.
rem
rem Usage:  tool\buildall.bat [DEBUG=3]
rem   (pass DEBUG=3 for the assert-dense testfixture; release otherwise.
rem    sqlite3.exe/.dll/procgen are always release.)
setlocal
set PATH=C:\Projects\sqlite;%PATH%
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Projects\sqlite

del /q sqlite3.c sqlite3.h testfixture.exe 2>nul
rmdir /s /q tsrc 2>nul

nmake /f Makefile.msc testfixture.exe %1
if errorlevel 1 exit /b 1
nmake /f Makefile.msc sqlite3.exe
if errorlevel 1 exit /b 2
nmake /f Makefile.msc sqlite3.dll
if errorlevel 1 exit /b 3
cl /nologo /O2 /I C:\Projects\sqlite /Fe:procgen.exe tool\procgen.c sqlite3.c
if errorlevel 1 exit /b 4

echo.
echo All four engine binaries rebuilt from the same source state:
for %%f in (testfixture.exe sqlite3.exe sqlite3.dll procgen.exe) do (
  for %%a in (%%f) do echo   %%f  %%~ta
)
exit /b 0
