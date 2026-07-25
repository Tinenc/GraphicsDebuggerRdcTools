# Self-verify Naraka inject helpers without launching the protected game.
# Run from repo root or from this folder:
#   powershell -ExecutionPolicy Bypass -File util\pe_import_inject\naraka_verify.ps1

[CmdletBinding()]
param(
  [string]$GameRoot = "F:\NarakaMobile\game",
  [switch]$RunPeSanity
)

$ErrorActionPreference = "Continue"
$failed = 0
function Ok($m) { Write-Host "[PASS] $m" -ForegroundColor Green }
function Bad($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; $script:failed++ }
function Info($m) { Write-Host "[INFO] $m" }

Write-Host "=== Naraka inject self-verify ==="

# --- 1. Mirror of sys_win32_hooks.cpp blacklist (keep in sync) ---
$blacklist = @(
  "tinecmatoolcmd.exe", "qtinecmatool.exe", "platformprocess.exe",
  "narakamobilelauncher.exe", "startgame_l22.exe",
  "yjneacclient.exe", "unitycrashhandler64.exe", "unicrashreporter.exe",
  "narakam_patcher.exe", "narakam_updater.exe",
  "elevate.exe", "uninst.exe", "p2pupdater.exe", "asar_replacer.exe", "xdelta3.exe",
  "ffmpeg.exe", "ccmini.exe", "ccvideoplayer.exe", "mliveccplayerapp.exe",
  "webview_support_browser.exe"
)

function Test-Blacklisted([string]$hay) {
  $h = $hay.ToLowerInvariant()
  foreach ($b in $blacklist) {
    if ($h.Contains($b)) { return $true }
  }
  if ($h.Contains("webviewsupport") -and $h.Contains("render.exe")) { return $true }
  if (($h.Contains("narakamobile") -or $h.Contains("narakabladepoint")) -and
      -not $h.Contains("narakabladepointmobile.exe")) { return $true }
  return $false
}

$cases = @(
  @{ hay = "F:\NarakaMobile\game\NarakaBladepointMobile.exe"; expect = $false; name = "game exe allowed" },
  @{ hay = "F:\NarakaMobile\NGP\NarakaMobileLauncher.exe"; expect = $true; name = "launcher blocked" },
  @{ hay = "F:\NarakaMobile\game\StartGame_l22.exe"; expect = $true; name = "startgame blocked (avoid stall)" },
  @{ hay = "F:\NarakaMobile\game\YJNeacClient.exe"; expect = $true; name = "NEAC client blocked" },
  @{ hay = "C:\Tools\qTinecmaTool.exe"; expect = $true; name = "UI self blocked" },
  @{ hay = "F:\NarakaMobile\game\webviewsupport.cef904430\render.exe"; expect = $true; name = "cef render blocked" },
  @{ hay = "C:\OtherApp\render.exe"; expect = $false; name = "unrelated render.exe allowed" }
)

foreach ($c in $cases) {
  $got = Test-Blacklisted $c.hay
  if ($got -eq $c.expect) { Ok $c.name }
  else { Bad "$($c.name) (got=$got expect=$($c.expect)) hay=$($c.hay)" }
}

# whitelist filter mimic (Global Hook path: game only)
$wl = "narakabladepointmobile.exe"
$pathPrefix = "f:\narakamobile\game"
function Pass-Filters([string]$hay) {
  $h = $hay.ToLowerInvariant()
  if (-not $h.Contains($pathPrefix)) { return $false }
  if (-not $h.Contains($wl)) { return $false }
  if (Test-Blacklisted $h) { return $false }
  return $true
}
if (Pass-Filters "F:\NarakaMobile\game\NarakaBladepointMobile.exe") {
  Ok "whitelist+pathPrefix allows game"
} else { Bad "whitelist+pathPrefix should allow game" }

if (-not (Pass-Filters "F:\NarakaMobile\game\StartGame_l22.exe")) {
  Ok "whitelist+pathPrefix blocks startgame"
} else { Bad "whitelist+pathPrefix should block startgame" }

if (-not (Pass-Filters "F:\NarakaMobile\game\ffmpeg.exe")) {
  Ok "whitelist+pathPrefix blocks ffmpeg"
} else { Bad "whitelist+pathPrefix should block ffmpeg" }

# --- 2. Game tree presence ---
$need = @(
  "NarakaBladepointMobile.exe", "GameAssembly.dll", "UnityPlayer.dll",
  "YJNeacClient.exe", "NeacInterface.dll", "NeacSafe64.sys"
)
foreach ($n in $need) {
  $p = Join-Path $GameRoot $n
  if (Test-Path -LiteralPath $p) { Ok "found $n" }
  else { Bad "missing $n under $GameRoot" }
}

$launcher = "F:\NarakaMobile\NGP\NarakaMobileLauncher.exe"
if (Test-Path -LiteralPath $launcher) { Ok "found launcher" }
else { Bad "missing launcher $launcher" }

# --- 3. Branch / source sanity ---
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$hooks = Join-Path $repo "renderdoc\os\win32\sys_win32_hooks.cpp"
if (Test-Path $hooks) {
  $txt = Get-Content $hooks -Raw
  if ($txt -match "narakabladepointmobile|narakamobilelauncher|TINECMATOOL_CHILD_WHITELIST") {
    Ok "sys_win32_hooks.cpp contains Naraka filters"
  } else {
    Bad "sys_win32_hooks.cpp missing Naraka filter tokens"
  }
  if ($txt -match "TINECMATOOL_USE_MANUALMAP|ManualMap" -or
      (Select-String -Path (Join-Path $repo "renderdoc\os\win32\win32_process.cpp") -Pattern "InjectDLL_ManualMap" -Quiet)) {
    Ok "manual-map inject present in win32_process.cpp"
  } else {
    Bad "manual-map inject missing"
  }
} else {
  Bad "hooks file missing"
}

# --- 4. Optional PE sanity on hello.exe (does not touch the game) ---
if ($RunPeSanity) {
  $pocDir = Join-Path $PSScriptRoot "poc_dll"
  Push-Location $pocDir
  try {
    cmd /c build.bat
    cmd /c build_hello.bat
  } finally { Pop-Location }

  $hello = Join-Path $pocDir "build\hello.exe"
  $pocDll = Join-Path $pocDir "build\TinecmaTool_PoC.dll"
  if ((Test-Path $hello) -and (Test-Path $pocDll)) {
    $sb = Join-Path $env:TEMP "tinecma_naraka_pe_test"
    New-Item $sb -ItemType Directory -Force | Out-Null
    Copy-Item $hello, $pocDll $sb -Force
    $out = Join-Path $sb "hello_patched.exe"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "inject.ps1") `
      -InputFile (Join-Path $sb "hello.exe") `
      -OutputFile $out `
      -InjectDll "TinecmaTool_PoC.dll" `
      -ImportSymbol "TnT_Entry"
    if (Test-Path $out) {
      & $out | Out-Null
      $logs = Get-ChildItem "$env:TEMP\TinecmaTool_PoC" -ErrorAction SilentlyContinue
      if ($logs) { Ok "PE import sanity produced PoC log" }
      else { Bad "PE import sanity: no PoC log under %TEMP%\TinecmaTool_PoC" }
    } else { Bad "PE import sanity: patched exe not produced" }
  } else {
    Bad "PE sanity skipped: build PoC/hello first (build.bat failed?)"
  }
} else {
  Info "Skip PE sanity (pass -RunPeSanity to enable)"
}

# --- 5. Build products (informational) ---
$dll = Join-Path $repo "x64\Development\TinecmaTool.dll"
$ui = Join-Path $repo "x64\Development\qTinecmaTool.exe"
if (Test-Path $dll) { Ok "built TinecmaTool.dll" } else { Info "TinecmaTool.dll not built yet" }
if (Test-Path $ui) { Ok "built qTinecmaTool.exe" } else { Info "qTinecmaTool.exe not built yet" }

Write-Host ""
if ($failed -eq 0) {
  Write-Host "ALL CHECKS PASSED" -ForegroundColor Green
  exit 0
} else {
  Write-Host "$failed CHECK(S) FAILED" -ForegroundColor Red
  exit 1
}
