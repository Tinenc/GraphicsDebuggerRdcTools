<#
.SYNOPSIS
    One-shot patcher for Wuthering Waves: inject TinecmaTool_PoC.dll into
    Client-Win64-ShippingBase.dll so the ntdll loader pulls it in during
    game start-up. Strictly a probing tool -- no D3D hooks, just confirms
    we get DllMain'd before the anti-cheat blocks us.

.DESCRIPTION
    Steps:
      1. Copy Client-Win64-ShippingBase.dll -> .dll.orig (idempotent).
      2. Copy TinecmaTool_PoC.dll into the game's Win64 dir.
      3. Run inject.ps1 on the .orig and write the patched DLL back to
         Client-Win64-ShippingBase.dll.
      4. Print exactly the launcher path you should now run by hand.

    After you launch the game by hand:
      * If %TEMP%\TinecmaTool_PoC\Client-Win64-Shipping.exe_<pid>.log
        appears, our DLL ran -- check PEB.Ldr for ACE-Base.dll.
      * If the game complains about file integrity / kicks you out,
        anti-tamper caught us. Restore originals with -Restore.

.PARAMETER GameDir
    Path to "...\Wuthering Waves Game\Client\Binaries\Win64".

.PARAMETER Restore
    Roll back: put .dll.orig back over Client-Win64-ShippingBase.dll and
    remove our staged TinecmaTool_PoC.dll. Always run this when you're
    done testing or you'll keep getting integrity errors.

.EXAMPLE
    .\wuwa_patch.ps1 -GameDir 'D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64'

.EXAMPLE
    .\wuwa_patch.ps1 -GameDir 'D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64' -Restore
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$here     = Split-Path -Parent $MyInvocation.MyCommand.Definition
$injectPs = Join-Path $here 'inject.ps1'
$pocBuild = Join-Path $here 'poc_dll\build\TinecmaTool_PoC.dll'

if(-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    throw "GameDir not found: $GameDir"
}
$base     = Join-Path $GameDir 'Client-Win64-ShippingBase.dll'
$baseOrig = "$base.orig"
$pocDest  = Join-Path $GameDir 'TinecmaTool_PoC.dll'

if($Restore) {
    Write-Host "[*] Restoring original Client-Win64-ShippingBase.dll ..."
    if(Test-Path -LiteralPath $baseOrig) {
        Copy-Item -LiteralPath $baseOrig -Destination $base -Force
        Remove-Item -LiteralPath $baseOrig -Force
        Write-Host "    restored from .orig"
    } else {
        Write-Host "    no .orig backup found at $baseOrig -- nothing to restore"
    }
    if(Test-Path -LiteralPath $pocDest) {
        Remove-Item -LiteralPath $pocDest -Force
        Write-Host "    removed staged TinecmaTool_PoC.dll"
    }
    # Wipe stale PoC logs so the next attempt starts clean.
    $logDir = Join-Path $env:TEMP 'TinecmaTool_PoC'
    if(Test-Path -LiteralPath $logDir) {
        Remove-Item -LiteralPath $logDir -Recurse -Force
        Write-Host "    wiped $logDir"
    }
    Write-Host "[+] Restore complete."
    return
}

if(-not (Test-Path -LiteralPath $injectPs))  { throw "Can't find inject.ps1 at $injectPs" }
if(-not (Test-Path -LiteralPath $pocBuild))  { throw "Build the PoC DLL first: cd poc_dll; .\build.bat" }
if(-not (Test-Path -LiteralPath $base))      { throw "Client-Win64-ShippingBase.dll not found in $GameDir" }

# 1) Idempotent backup.
if(-not (Test-Path -LiteralPath $baseOrig)) {
    Copy-Item -LiteralPath $base -Destination $baseOrig
    Write-Host "[*] Backed up Client-Win64-ShippingBase.dll -> .orig"
} else {
    Write-Host "[*] .orig backup already present, keeping it"
}

# 2) Stage PoC DLL next to the game exe.
Copy-Item -LiteralPath $pocBuild -Destination $pocDest -Force
Write-Host "[*] Staged TinecmaTool_PoC.dll -> $pocDest"

# 3) Run patcher on the *backup* (clean source), write to the live name.
Write-Host "[*] Patching Client-Win64-ShippingBase.dll ..."
& powershell -ExecutionPolicy Bypass -File $injectPs `
    -InputFile    $baseOrig `
    -OutputFile   $base `
    -InjectDll    'TinecmaTool_PoC.dll' `
    -ImportSymbol 'TnT_Entry' | Tee-Object -Variable patchOut | Out-Null
foreach($l in $patchOut) { Write-Host "    $l" }

# 4) Tell the user what to do next.
$logDir = Join-Path $env:TEMP 'TinecmaTool_PoC'
if(Test-Path -LiteralPath $logDir) { Remove-Item -LiteralPath $logDir -Recurse -Force }

Write-Host ""
Write-Host "[+] Done. Now:"
Write-Host "    1) Launch the game normally (double-click launcher, NOT via qTinecmaTool)."
Write-Host "    2) When you'\''re past the title screen (or it crashes), check:"
Write-Host "         dir `"$logDir`""
Write-Host "       Look for Client-Win64-Shipping.exe_*.log -- that file appearing means"
Write-Host "       our DLL got loaded by ntdll. The PEB.Ldr dump inside tells us where"
Write-Host "       ACE-Base.dll sits relative to us."
Write-Host ""
Write-Host "[!] When done testing, ALWAYS run with -Restore so anti-cheat doesn'\''t catch"
Write-Host "    a tampered Client-Win64-ShippingBase.dll on a real session:"
Write-Host "        .\wuwa_patch.ps1 -GameDir '$GameDir' -Restore"
