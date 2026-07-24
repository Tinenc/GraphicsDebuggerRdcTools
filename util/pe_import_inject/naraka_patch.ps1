# NarakaMobile PE Import patch / restore wrapper
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\naraka_patch.ps1 -Action probe
#   powershell -ExecutionPolicy Bypass -File .\naraka_patch.ps1 -Action patch -InjectDll "C:\path\TinecmaTool.dll"
#   powershell -ExecutionPolicy Bypass -File .\naraka_patch.ps1 -Action restore
#
# Default game root: F:\NarakaMobile\game
# Target EXE:        NarakaBladepointMobile.exe

[CmdletBinding()]
param(
  [ValidateSet("probe", "patch", "restore")]
  [string]$Action = "probe",

  [string]$GameRoot = "F:\NarakaMobile\game",

  [string]$TargetExe = "NarakaBladepointMobile.exe",

  # Full path to the DLL that will be written into the PE import table.
  # For a real capture this should be TinecmaTool.dll (or a thin trampoline).
  [string]$InjectDll = "",

  [string]$ImportSymbol = "INTERNAL_SetCaptureOptions"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$injectPs1 = Join-Path $here "inject.ps1"
$targetPath = Join-Path $GameRoot $TargetExe
$backupPath = Join-Path $GameRoot ($TargetExe + ".tinecma.bak")

function Assert-File([string]$p, [string]$label) {
  if (-not (Test-Path -LiteralPath $p)) {
    throw "$label not found: $p"
  }
}

Write-Host "=== NarakaMobile PE import $Action ==="
Write-Host "GameRoot : $GameRoot"
Write-Host "Target   : $targetPath"

Assert-File $targetPath "Target EXE"

switch ($Action) {
  "probe" {
    $fi = Get-Item -LiteralPath $targetPath
    Write-Host ("Size     : {0:N0} bytes" -f $fi.Length)
    Write-Host ("Backup   : {0} (exists={1})" -f $backupPath, (Test-Path -LiteralPath $backupPath))

    $companions = @(
      "YJNeacClient.exe", "NeacInterface.dll", "NeacSafe64.sys",
      "GameAssembly.dll", "UnityPlayer.dll", "StartGame_l22.exe",
      "CrashHunter_PC3.dll"
    )
    Write-Host "Companions:"
    foreach ($c in $companions) {
      $p = Join-Path $GameRoot $c
      $ok = Test-Path -LiteralPath $p
      Write-Host ("  [{0}] {1}" -f ($(if ($ok) { "OK" } else { "--" }), $c))
    }

    Write-Host ""
    Write-Host "Recommended capture target (no PE patch):"
    Write-Host "  $targetPath"
    Write-Host "Working directory:"
    Write-Host "  $GameRoot"
    Write-Host ""
    Write-Host "If using Global Hook, set before launching qTinecmaTool.exe:"
    Write-Host '  $env:TINECMATOOL_CHILD_WHITELIST = "narakabladepointmobile.exe"'
    Write-Host '  $env:TINECMATOOL_CHILD_PATH_PREFIX = "f:\narakamobile\game"'
  }

  "patch" {
    if ([string]::IsNullOrWhiteSpace($InjectDll)) {
      throw " -InjectDll is required for -Action patch"
    }
    Assert-File $InjectDll "Inject DLL"
    Assert-File $injectPs1 "inject.ps1"

    if (-not (Test-Path -LiteralPath $backupPath)) {
      Copy-Item -LiteralPath $targetPath -Destination $backupPath -Force
      Write-Host "Backup created: $backupPath"
    }
    else {
      Write-Host "Backup already exists, keeping it: $backupPath"
    }

    $dllName = Split-Path -Leaf $InjectDll
    $dllDest = Join-Path $GameRoot $dllName
    $srcFull = (Resolve-Path -LiteralPath $InjectDll).Path
    $dstFull = $dllDest
    if ($srcFull -ne $dstFull) {
      Copy-Item -LiteralPath $InjectDll -Destination $dllDest -Force
      Write-Host "Copied DLL -> $dllDest"
    }

    $patched = Join-Path $GameRoot ($TargetExe + ".tinecma.patched")
    & powershell -ExecutionPolicy Bypass -File $injectPs1 `
      -InputFile $backupPath `
      -OutputFile $patched `
      -InjectDll $dllName `
      -ImportSymbol $ImportSymbol

    if ($LASTEXITCODE -ne 0) {
      throw "inject.ps1 failed with exit $LASTEXITCODE"
    }

    Copy-Item -LiteralPath $patched -Destination $targetPath -Force
    Remove-Item -LiteralPath $patched -Force -ErrorAction SilentlyContinue
    Write-Host "Patched in place: $targetPath"
    Write-Host "Restore with: -Action restore"
  }

  "restore" {
    Assert-File $backupPath "Backup"
    Copy-Item -LiteralPath $backupPath -Destination $targetPath -Force
    Write-Host "Restored $targetPath from $backupPath"
  }
}
