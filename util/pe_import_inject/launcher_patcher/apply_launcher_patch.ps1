#!/usr/bin/env pwsh
<#
TinecmaTool launcher_main.dll patch installer.

Backs up the original launcher_main.dll, drops the patched version in its place,
then waits for the user to confirm before either keeping the patch or rolling
back. Designed to be paranoid: any anomaly during the test launch can be undone
with a single keypress.

Usage:
  .\apply_launcher_patch.ps1                          # install + interactive confirm
  .\apply_launcher_patch.ps1 -Restore                 # restore from backup, no questions
  .\apply_launcher_patch.ps1 -Status                  # show current state only
#>
[CmdletBinding(DefaultParameterSetName='Install')]
param(
    [Parameter(ParameterSetName='Install')]
    [string]$LauncherDir = 'C:\Wuthering Waves\2.6.2.0',

    [Parameter(ParameterSetName='Install')]
    [string]$PatchedDll  = 'C:\Program Files\GraphicsDebuggerRdcTools\util\pe_import_inject\launcher_decompile\launcher_main.patched.dll',

    [Parameter(ParameterSetName='Restore')]
    [switch]$Restore,

    [Parameter(ParameterSetName='Status')]
    [switch]$Status
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $LauncherDir 'launcher_main.dll'
$backup = Join-Path $LauncherDir 'launcher_main.dll.tinecmatool.bak'

function Get-Md5([string]$path) {
    if (-not (Test-Path $path)) { return '<missing>' }
    (Get-FileHash $path -Algorithm MD5).Hash.ToLower()
}

function Show-Status {
    Write-Host ''
    Write-Host '=== launcher_main.dll status ===' -ForegroundColor Cyan
    Write-Host ('  target  : {0}' -f $target)
    if (Test-Path $target) {
        Write-Host ('              size = {0,10}   md5 = {1}' -f (Get-Item $target).Length, (Get-Md5 $target))
    } else {
        Write-Host '              <missing>' -ForegroundColor Red
    }
    Write-Host ('  backup  : {0}' -f $backup)
    if (Test-Path $backup) {
        Write-Host ('              size = {0,10}   md5 = {1}' -f (Get-Item $backup).Length, (Get-Md5 $backup))
    } else {
        Write-Host '              <no backup yet>' -ForegroundColor Yellow
    }
}

if ($Status) {
    Show-Status
    return
}

if ($Restore) {
    if (-not (Test-Path $backup)) {
        Write-Host '[!] no backup found, nothing to restore.' -ForegroundColor Red
        return
    }
    Write-Host '[*] restoring original launcher_main.dll from backup...' -ForegroundColor Cyan
    Copy-Item $backup $target -Force
    Show-Status
    Write-Host '[+] restored.' -ForegroundColor Green
    return
}

# Install path
if (-not (Test-Path $target))     { throw "target not found: $target" }
if (-not (Test-Path $PatchedDll)) { throw "patched dll not found: $PatchedDll" }

if (-not (Test-Path $backup)) {
    Write-Host '[*] creating backup of original launcher_main.dll...' -ForegroundColor Cyan
    Copy-Item $target $backup -Force
} else {
    Write-Host '[*] backup already exists, leaving it untouched.' -ForegroundColor Yellow
}

Write-Host '[*] installing patched launcher_main.dll...' -ForegroundColor Cyan
Copy-Item $PatchedDll $target -Force
Show-Status

Write-Host ''
Write-Host '=== NEXT STEPS ===' -ForegroundColor Magenta
Write-Host '  1) Launch the WuWa launcher (double-click launcher.exe in C:\Wuthering Waves).'
Write-Host '  2) Observe whether the "游戏文件缺失" dialog appears, whether the launcher'
Write-Host '     self-updates, or whether the green "启动游戏" button is available.'
Write-Host '  3) Take a screenshot / note the symptoms, but do NOT click any "立即修复" or'
Write-Host '     update button.'
Write-Host ''
Write-Host '  After observing, come back here and either:'
Write-Host '     - Type Y + Enter to KEEP the patch (only safe if everything looked normal)'
Write-Host '     - Type anything else to ROLLBACK to the original launcher_main.dll'
Write-Host ''
$answer = Read-Host 'Keep patched launcher_main.dll? [y/N]'
if ($answer -match '^[yY]') {
    Write-Host '[+] keeping patched build.' -ForegroundColor Green
    Show-Status
} else {
    Write-Host '[*] rolling back...' -ForegroundColor Yellow
    Copy-Item $backup $target -Force
    Show-Status
    Write-Host '[+] rolled back.' -ForegroundColor Green
}
