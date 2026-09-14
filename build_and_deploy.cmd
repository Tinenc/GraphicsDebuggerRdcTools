@echo off
setlocal enabledelayedexpansion

rem ============================================================================
rem  TinecmaTool - build from the tree that ships   (branch: tinecmatool/mumu)
rem
rem  This tree (C:\Program Files\GraphicsDebuggerRdcTools) is both the source
rem  checkout AND the deploy target: renderdoc.vcxproj sets
rem    OutDir = $(SolutionDir)$(Platform)\$(Configuration)\
rem  so building here writes straight into x64\Development / Win32\Development.
rem  There is NO robocopy step and no second copy to keep in sync.
rem
rem  PLATFORMS - this script builds BOTH:
rem    x64  -> x64\Development\    (TinecmaTool.dll, TinecmaToolshim64.dll, ...)
rem            64-bit side. This is what the MuMu capture path needs.
rem    x86  -> Win32\Development\  (TinecmaTool.dll, TinecmaToolshim32.dll, ...)
rem            32-bit side, only used when attaching to a 32-bit target.
rem  The solution maps platform "x86" onto each project's "Win32" config.
rem
rem    build_and_deploy.cmd          build x64 + x86
rem    build_and_deploy.cmd nox86    build x64 only
rem
rem  x86 failure is NOT fatal - the x64 output is what matters, and the 32-bit
rem  half is optional. The script reports it and carries on.
rem
rem  BRANDING - every output in this tree is named TinecmaTool, not RenderDoc:
rem      x64\Development\  TinecmaTool.dll / TinecmaToolshim64.dll / TinecmaToolcmd.exe / qTinecmaTool.exe
rem      Win32\Development\ TinecmaTool.dll / TinecmaToolshim32.dll / TinecmaToolcmd.exe
rem  This is deliberate, not cosmetic: anti-cheat on the capture target fingerprints
rem  RenderDoc by module name, helper-exe name, shim name, the replay marker export,
rem  named kernel objects and the window class. See tools\branding_tinecmatool.py for
rem  the authoritative rule table, and BRANDING_TINECMATOOL.md for the mapping spec.
rem
rem  The -t: names below are SOLUTION project names and deliberately still say
rem  "renderdoc", because the .sln entries were left alone. They only ever show up in
rem  the IDE; the binaries they produce carry the TinecmaTool names. Keep the two in
rem  sync if you ever rename the solution entries.
rem
rem  Do NOT hand-edit any name here without re-running the rebranding tool: the
rem  identity strings are PAIRED across files (renderdoc_replay.h exports the replay
rem  marker, win32_libentry.cpp looks for it via RDOC_BASE_NAME, crash_handler.h and
rem  renderdoccmd_win32.cpp share a kernel event name, renderdocshim.h and
rem  win32_process.cpp share the shim DLL name and the file-mapping name). Renaming
rem  one side without the other breaks self-detection or the injection path.
rem
rem  Fixes carried by this tree:
rem    1. globalconfig.h     - target control port pool 8 -> 64 (38920..38983)
rem    2. win32_process.cpp - AppInit_DLLs stores a dir-shortened but real-file
rem       path and verifies it resolves back to the shim before writing; and it
rem       skips the WOW64 half when the 32-bit shim is missing.
rem    3. Vulkan layer identity - the layer is VK_LAYER_TINECMATOOL_Capture with
rem       manifest TinecmaTool.json, NOT the upstream VK_LAYER_RENDERDOC_Capture /
rem       renderdoc.json, so a machine that also has the official RenderDoc
rem       installed does not have the two builds evict each other from
rem       HKLM\...\Khronos\Vulkan\ImplicitLayers.
rem    4. TinecmaTool rebranding - see above.
rem
rem  The script refuses to build when the sources lack the fixes, and refuses to
rem  report success when the built DLL does not contain them either.
rem  Run from an elevated prompt.
rem ============================================================================

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

set "BUILD_X86=1"
set "X86_FAILED=0"
if /i "%~1"=="nox86" set "BUILD_X86=0"

set "MSB=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%MSB%" set "MSB=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSB%" (
  echo [ERROR] MSBuild.exe not found. Edit the MSB variable in this script.
  exit /b 1
)

echo ============================================================
echo [0/4] Pre-build source check
echo ============================================================
findstr /C:"GetAppInitShimPath" "%REPO%\renderdoc\os\win32\win32_process.cpp" >nul
if errorlevel 1 (
  echo [ERROR] win32_process.cpp does NOT contain the AppInit shim fix.
  echo         You are building a tree without the fix - aborting.
  exit /b 1
)
findstr /C:"RenderDoc_FirstTargetControlPort + 63" "%REPO%\renderdoc\common\globalconfig.h" >nul
if errorlevel 1 (
  echo [ERROR] globalconfig.h does NOT contain the 64-slot port pool.
  echo         You are building a tree without the fix - aborting.
  exit /b 1
)
findstr /C:"VK_LAYER_TINECMATOOL_Capture" "%REPO%\renderdoc\common\globalconfig.h" >nul
if errorlevel 1 (
  echo [ERROR] globalconfig.h still uses the upstream VK_LAYER_RENDERDOC_Capture.
  echo         This build would collide with an official RenderDoc install - aborting.
  exit /b 1
)
findstr /C:"RENDERDOC_VULKAN_LAYER_JSON_BASENAME" "%REPO%\renderdoc\common\globalconfig.h" >nul
if errorlevel 1 (
  echo [ERROR] globalconfig.h has no TinecmaTool layer-manifest basename - aborting.
  exit /b 1
)
findstr /C:"TinecmaTool.json" "%REPO%\renderdoc\renderdoc.vcxproj" >nul
if errorlevel 1 (
  echo [ERROR] renderdoc.vcxproj still writes the layer manifest as ProjectName.json.
  echo         Runtime GetJSONPath would then look for a file that is never built.
  exit /b 1
)
echo   sources OK: AppInit fix present, port pool = 64 slots,
echo               layer identity = VK_LAYER_TINECMATOOL_Capture / TinecmaTool.json

rem Branding check. The replay marker string is exported by renderdoc_replay.h and
rem looked up by win32_libentry.cpp through RDOC_BASE_NAME; if the two ever drift the
rem DLL stops recognising its own replay/helper processes and injects into itself.
findstr /C:"TinecmaTool__replay__marker" "%REPO%\renderdoc\api\replay\renderdoc_replay.h" >nul
if errorlevel 1 (
  echo [ERROR] renderdoc_replay.h does not export TinecmaTool__replay__marker.
  echo         The DLL would not recognise its own helper processes - aborting.
  exit /b 1
)
findstr /C:"#define RDOC_BASE_NAME TinecmaTool" "%REPO%\renderdoc\os\win32\win32_process.cpp" >nul
if errorlevel 1 (
  echo [ERROR] win32_process.cpp does not define RDOC_BASE_NAME as TinecmaTool.
  echo         Module name and replay marker would disagree - aborting.
  exit /b 1
)
findstr /C:"TinecmaToolshim64.dll" "%REPO%\renderdocshim\renderdocshim.h" >nul
if errorlevel 1 (
  echo [ERROR] renderdocshim.h still declares the shim under the old name.
  echo         The AppInit path would point at a DLL the build never produces - aborting.
  exit /b 1
)
echo               branding = TinecmaTool (marker, shim, RDOC_BASE_NAME all agree)

echo.
echo ============================================================
echo [1/4] Building x64 Development
echo ============================================================
"%MSB%" "%REPO%\renderdoc.sln" -t:renderdoc -t:renderdoccmd -t:renderdocshim -t:qrenderdoc ^
        -p:Configuration=Development -p:Platform=x64 -m -v:m -nologo
if errorlevel 1 goto :fail
echo   [OK] x64 build finished.

echo.
echo ============================================================
echo [2/4] Building Win32 Development (32-bit side)
echo ============================================================
if "%BUILD_X86%"=="0" (
  echo   skipped: "nox86" was passed.
  goto :verify
)
"%MSB%" "%REPO%\renderdoc.sln" -t:renderdoc -t:renderdoccmd -t:renderdocshim -t:qrenderdoc ^
        -p:Configuration=Development -p:Platform=x86 -m -v:m -nologo
if errorlevel 1 (
  set "X86_FAILED=1"
  echo.
  echo   [WARN] x86 build FAILED. Continuing anyway - the x64 output above is
  echo          unaffected and is the side the MuMu capture path uses.
  echo          Re-run without "nox86" once you want 32-bit attach back.
) else (
  echo   [OK] x86 build finished.
)

:verify
echo.
echo ============================================================
echo [3/4] Verifying the BUILT binaries really contain the fixes
echo ============================================================
set "BIN64=%REPO%\x64\Development\TinecmaTool.dll"
set "BIN32=%REPO%\Win32\Development\TinecmaTool.dll"

if not exist "%BIN64%" (
  echo [FAIL] %BIN64% was not produced.
  goto :fail
)
findstr /C:"Refusing to write an AppInit_DLLs value" "%BIN64%" >nul
if errorlevel 1 (
  echo [FAIL] %BIN64% does NOT contain the new AppInit guard string.
  echo        The build did not pick up the fix - check for a stale obj dir.
  goto :fail
)
echo   [OK] x64 TinecmaTool.dll contains the new AppInit guard

set "JSON64=%REPO%\x64\Development\TinecmaTool.json"
if not exist "%JSON64%" (
  echo [FAIL] %JSON64% was not produced by the build.
  echo        The layer manifest target did not run - ImplicitLayers would point
  echo        at a file that does not exist.
  goto :fail
)
findstr /C:"VK_LAYER_TINECMATOOL_Capture" "%JSON64%" >nul
if errorlevel 1 (
  echo [FAIL] %JSON64% does not declare VK_LAYER_TINECMATOOL_Capture.
  echo        It would register under the upstream name and collide with the
  echo        official RenderDoc install.
  goto :fail
)
findstr /C:"ENABLE_VULKAN_TINECMATOOL_CAPTURE" "%JSON64%" >nul
if errorlevel 1 (
  echo [FAIL] %JSON64% does not use ENABLE_VULKAN_TINECMATOOL_CAPTURE.
  goto :fail
)
findstr /C:"VK_LAYER_TINECMATOOL_Capture" "%BIN64%" >nul
if errorlevel 1 (
  echo [FAIL] %BIN64% does not export the VK_LAYER_TINECMATOOL_Capture entry points.
  echo        The manifest advertises symbols the DLL does not have - check that
  echo        driver\vulkan\vk_layer.cpp was rebuilt.
  goto :fail
)
echo   [OK] x64 layer manifest and DLL entry points agree

rem Branding must be verifiable in the BUILT module, not just in the sources: an
rem incremental build that did not recompile renderdoc_replay.h or renderdocshim.h
rem silently produces an old-named binary, which is exactly the thing that gets the
rem process flagged.
findstr /C:"TinecmaTool__replay__marker" "%BIN64%" >nul
if errorlevel 1 (
  echo [FAIL] %BIN64% does not export TinecmaTool__replay__marker.
  echo        Either the rebranding was not applied or the build is stale.
  echo        Do a clean rebuild of renderdoc.vcxproj.
  goto :fail
)
findstr /C:"TinecmaToolGlobalHookData" "%BIN64%" >nul
if errorlevel 1 (
  echo [FAIL] %BIN64% does not use TinecmaToolGlobalHookData - stale build.
  goto :fail
)
findstr /C:"TINECMATOOL_CRASHHANDLE" "%BIN64%" >nul
if errorlevel 1 (
  echo [FAIL] %BIN64% does not use TINECMATOOL_CRASHHANDLE - stale build.
  goto :fail
)
echo   [OK] x64 TinecmaTool.dll carries the TinecmaTool marker / shim / crash-event names

if exist "%REPO%\x64\Development\renderdoc.dll" (
  echo   [WARN] stale x64\Development\renderdoc.dll is still there. It is the
  echo          pre-rebranding module name and must not be loaded any more -
  echo          delete it, or do a clean rebuild.
)
if exist "%REPO%\x64\Development\renderdocshim64.dll" (
  echo   [WARN] stale x64\Development\renderdocshim64.dll is still there - delete it.
)
if exist "%REPO%\x64\Development\renderdoccmd.exe" (
  echo   [WARN] stale x64\Development\renderdoccmd.exe is still there - delete it.
)
if exist "%REPO%\x64\Development\qrenderdoc.exe" (
  echo   [WARN] stale x64\Development\qrenderdoc.exe is still there - delete it.
)
if exist "%REPO%\x64\Development\renderdocui.exe" (
  echo   [WARN] stale x64\Development\renderdocui.exe is still there - delete it.
)

if exist "%REPO%\x64\Development\renderdoc.json" (
  echo   [WARN] stale x64\Development\renderdoc.json is still there. It is the
  echo          upstream-named manifest and must not be registered any more -
  echo          delete it, or do a clean rebuild.
)
if exist "%REPO%\Win32\Development\renderdoc.json" (
  echo   [WARN] stale Win32\Development\renderdoc.json is still there - delete it.
)

if not exist "%BIN32%" (
  echo   [WARN] %BIN32% was not produced - 32-bit attach will be unavailable.
) else (
  findstr /C:"Refusing to write an AppInit_DLLs value" "%BIN32%" >nul
  if errorlevel 1 (
    echo   [WARN] %BIN32% does NOT contain the new AppInit guard string.
  ) else (
    echo   [OK] Win32 TinecmaTool.dll contains the new AppInit guard
  )
)

echo.
echo ============================================================
echo [4/4] Deploy folder + state check
echo ============================================================
echo Global hook shims the AppInit_DLLs entry will point at:
if exist "%REPO%\x64\Development\TinecmaToolshim64.dll" (echo   [OK] x64\Development\TinecmaToolshim64.dll) else (echo   [MISSING] x64\Development\TinecmaToolshim64.dll)
if exist "%REPO%\Win32\Development\TinecmaToolshim32.dll" (echo   [OK] Win32\Development\TinecmaToolshim32.dll) else (echo   [MISSING] Win32\Development\TinecmaToolshim32.dll)

echo.
echo AppInit_DLLs must be EMPTY while no global hook is running:
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows" /v AppInit_DLLs 2>nul
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Windows" /v AppInit_DLLs 2>nul

echo.
echo Vulkan implicit-layer entries belonging to a RenderDoc-family build.
echo After the cleanup only "...\TinecmaTool.json" of THIS tree may remain:
echo   --- 64-bit ---
reg query "HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers" 2>nul | findstr /I "renderdoc tinecma rendertest rtcap"
echo   --- 32-bit ---
reg query "HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers" 2>nul | findstr /I "renderdoc tinecma rendertest rtcap"
echo.
echo To purge dead / upstream-named entries, run elevated:
echo     python "%REPO%\fix_vulkan_layer_registration.py" --apply

echo.
echo ============================================================
echo DONE.
echo ============================================================
echo Built: x64 = yes    x86 = %BUILD_X86%
if "%X86_FAILED%"=="1" echo WARNING: the x86 build failed - see [2/4] above.
echo Next: start qTinecmaTool.exe and attach to
echo   D:\MuMuPlayer\nx_device\15.0\shell\MuMuNxDevice.exe
echo with "hook into children" enabled, then boot the emulator instance.
echo Vulkan layer this build registers:  VK_LAYER_TINECMATOOL_Capture
echo   manifest: %REPO%\x64\Development\TinecmaTool.json
echo   verify: a Vulkan app's layer list must show TINECMATOOL, and the official
echo    RenderDoc entry at C:\Program Files\RenderDoc must stay intact.
echo Success looks like, in %TEMP%\TinecmaTool\TinecmaTool_^<ts^>.log:
echo   1. APIs in %%APPDATA%%\TinecmaTool\analytics.json is no longer []
echo   2. "Got remote handshake: MuMuNxDevice"
echo   3. "Adding OpenGLES device frame capturer" for PID of MuMuNxDevice
echo      - this is the new one. Without it only "Adding Vulkan device frame
echo        capturer" appears and the capture holds just the ~8-draw present frame.
echo   4. the captured frame shows a real drawcall count (hundreds+), not 8
exit /b 0

:fail
echo.
echo [FAILED] see the output above.
exit /b 1
