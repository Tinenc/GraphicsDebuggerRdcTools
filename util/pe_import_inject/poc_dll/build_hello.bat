@echo off
setlocal enabledelayedexpansion
if "%VCINSTALLDIR%"=="" (
    set "_vcvars=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    call "!_vcvars!" x64 || exit /b 1
)
pushd "%~dp0"
if not exist build mkdir build
pushd build
cl /nologo /MD /Zi /EHsc ..\hello.cpp /link /OUT:hello.exe /PDB:hello.pdb
set _rc=%ERRORLEVEL%
popd
popd
exit /b %_rc%
