@echo off
REM Build TinecmaTool_PoC.dll (x64) using the MSVC compiler shipped with
REM VS 2022 Community. Run from a Developer Command Prompt for VS 2022, or
REM let this script find vcvarsall.bat itself.

setlocal enabledelayedexpansion

if "%VCINSTALLDIR%"=="" (
    set "_vcvars=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    if not exist "!_vcvars!" (
        set "_vcvars=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    )
    if not exist "!_vcvars!" (
        set "_vcvars=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    )
    if not exist "!_vcvars!" (
        echo Couldn't find vcvarsall.bat. Run this script from a Developer
        echo Command Prompt for VS 2022, or set VCINSTALLDIR first.
        exit /b 1
    )
    call "!_vcvars!" x64 || exit /b 1
)

pushd "%~dp0"
if not exist build mkdir build
pushd build

cl /nologo /LD /Zi /O1 /MD /std:c++17 /EHsc ^
   /D_WIN32_WINNT=0x0A00 ^
   ..\TinecmaTool_PoC.cpp ^
   /link /OUT:TinecmaTool_PoC.dll /PDB:TinecmaTool_PoC.pdb ^
   user32.lib kernel32.lib advapi32.lib

set _rc=%ERRORLEVEL%
popd
popd
exit /b %_rc%
