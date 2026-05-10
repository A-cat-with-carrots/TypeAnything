#requires -RunAsAdministrator
<#
Install TypeAnything: custom rime.dll + Weasel UI binaries + schema + rebrand.

Run from admin PowerShell:
  cd D:\hrdai\aiForType
  .\install-typeanything-to-weasel.ps1 -ApiKey "sk-xxxx"
#>

param(
  [Parameter(Mandatory=$true)]
  [string]$ApiKey,

  [string]$WeaselDir   = "C:\Program Files\Rime\weasel-0.17.4",
  [string]$BuildRoot   = "D:\hrdai\aiForType\third_party\weasel",
  [string]$OurRimeDll  = "D:\hrdai\aiForType\third_party\weasel\librime\dist\lib\rime.dll",
  [string]$SchemaSrc   = "D:\hrdai\aiForType\third_party\weasel\librime\plugins\typeanything\schema\typeanything.schema.yaml",
  [string]$RimeUserDir = (Join-Path $env:APPDATA "Rime"),
  [string]$TargetLang  = "English"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $WeaselDir)) { throw "Weasel not found at $WeaselDir." }
if (-not (Test-Path $OurRimeDll)) { throw "Our rime.dll not found: $OurRimeDll. Build first." }
if (-not (Test-Path $SchemaSrc))  { throw "Schema not found: $SchemaSrc" }

$WeaselClsid = "{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}"
$ProfileGuid = "{3D02CAB6-2B8E-4781-BA20-1C9267529467}"

Write-Host "==> Stop Weasel processes"
# 1. Graceful quit ONLY if a server is actually running. /q without a live
#    server hangs forever waiting for an IPC reply.
$existingServer = Join-Path $WeaselDir "WeaselServer.exe"
$serverRunning = Get-Process -Name "WeaselServer" -ErrorAction SilentlyContinue
if ($serverRunning -and (Test-Path $existingServer)) {
    Write-Host "    sending /q to running server..."
    $job = Start-Job -ScriptBlock {
        param($exe) & cmd.exe /c "`"$exe`" /q 2>nul" | Out-Null
    } -ArgumentList $existingServer
    Wait-Job $job -Timeout 5 | Out-Null
    Remove-Job $job -Force -ErrorAction SilentlyContinue
}
# 2. Force-kill survivors via taskkill. Suppress stderr at cmd level (PS5.1
#    wraps native stderr as ErrorRecord under ErrorActionPreference=Stop).
foreach ($proc in @("WeaselServer.exe", "WeaselDeployer.exe", "WeaselTrayIcon.exe")) {
    & cmd.exe /c "taskkill /F /IM $proc /T 1>nul 2>nul" | Out-Null
}
Start-Sleep -Seconds 2
# 3. Verify no stragglers.
$still = Get-Process -Name "WeaselServer", "WeaselDeployer" -ErrorAction SilentlyContinue
if ($still) {
    Write-Warning "    Could not kill: $($still | ForEach-Object { '{0}({1})' -f $_.Name, $_.Id })"
    Write-Warning "    Sign out + back in to fully release locks before retrying."
} else {
    Write-Host "    all Weasel processes stopped"
}

Write-Host "==> Backup official binaries"
foreach ($name in @("rime.dll", "weaselx64.dll", "weasel.dll", "WeaselServer.exe", "WeaselDeployer.exe")) {
    $orig = Join-Path $WeaselDir $name
    $bak  = "$orig.bak"
    if ((Test-Path $orig) -and -not (Test-Path $bak)) {
        Copy-Item $orig $bak -Force
        Write-Host "    backed up $name"
    }
}

Write-Host "==> Replace binaries with our build"

# Win32 MoveFileEx for pending-on-reboot replacement when file is in use.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class MoveEx {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileExW(string src, string dst, uint flags);
    public const uint MOVEFILE_REPLACE_EXISTING = 0x1;
    public const uint MOVEFILE_DELAY_UNTIL_REBOOT = 0x4;
}
'@

function Copy-LockedOrPending {
    param($Src, $Dst, $Label)
    # 1. Try direct copy with retry loop.
    for ($i = 0; $i -lt 8; $i++) {
        try {
            Copy-Item $Src $Dst -Force -ErrorAction Stop
            Write-Host "    copied $Label"
            return $true
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    # 2. Fall back: stage to a temp file next to dst, queue MoveFileEx for next boot.
    $temp = "$Dst.new"
    try {
        Copy-Item $Src $temp -Force -ErrorAction Stop
    } catch {
        Write-Warning "    cannot stage $Label even to temp ($temp): $_"
        return $false
    }
    if ([MoveEx]::MoveFileExW($temp, $Dst, [MoveEx]::MOVEFILE_REPLACE_EXISTING -bor [MoveEx]::MOVEFILE_DELAY_UNTIL_REBOOT)) {
        Write-Warning "    $Label LOCKED — staged to $temp; will replace on next reboot"
        $script:RebootNeeded = $true
        return $true
    } else {
        Write-Warning "    $Label LOCKED and MoveFileEx failed. Sign out + sign in, then re-run."
        return $false
    }
}

$script:RebootNeeded = $false

Copy-LockedOrPending $OurRimeDll (Join-Path $WeaselDir "rime.dll") "rime.dll" | Out-Null
$buildBin = Join-Path $BuildRoot "build\windows\x64\release"
foreach ($pair in @(
    @("WeaselTSF\weaselx64.dll",       "weaselx64.dll"),
    @("WeaselServer\WeaselServer.exe", "WeaselServer.exe"),
    @("WeaselDeployer\WeaselDeployer.exe", "WeaselDeployer.exe")
)) {
    $src = Join-Path $buildBin $pair[0]
    $dst = Join-Path $WeaselDir $pair[1]
    if (Test-Path $src) {
        Copy-LockedOrPending $src $dst $pair[1] | Out-Null
    } else {
        Write-Warning "    missing build artifact: $src"
    }
}

# TSF reads the language-bar IconFile from a separate copy in system32\weasel.dll
# (registered at install time by Weasel's MSI). Update it too so the tray icon
# becomes the angelfish, not the original Weasel glyph.
$sys32Dll = "C:\WINDOWS\system32\weasel.dll"
$sys32Bak = "$sys32Dll.bak"
$srcTSF = Join-Path $buildBin "WeaselTSF\weaselx64.dll"
if ((Test-Path $sys32Dll) -and -not (Test-Path $sys32Bak)) {
    Copy-Item $sys32Dll $sys32Bak -Force
}
if (Test-Path $srcTSF) {
    Copy-LockedOrPending $srcTSF $sys32Dll "system32\weasel.dll" | Out-Null
}

Write-Host "==> Hide other schemas from Deployer 'Plan list'"
$weaselData = Join-Path $WeaselDir "data"
$backupDir = Join-Path $WeaselDir "data.original"
if ((Test-Path $weaselData) -and -not (Test-Path $backupDir)) {
    Copy-Item -Recurse -Force $weaselData $backupDir
}
if (Test-Path $weaselData) {
    Get-ChildItem -Path $weaselData -Filter "*.schema.yaml" -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force
    }
    Get-ChildItem -Path $weaselData -Filter "*.dict.yaml" -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -notmatch '^luna_pinyin') { Remove-Item $_.FullName -Force }
    }
}

Write-Host "==> Install typeanything schema with API key"
if (-not (Test-Path $RimeUserDir)) { New-Item -ItemType Directory -Path $RimeUserDir | Out-Null }
$schemaContent = Get-Content $SchemaSrc -Raw -Encoding UTF8
$schemaContent = $schemaContent -replace 'api_key: ""', "api_key: ""$ApiKey"""
$schemaContent = $schemaContent -replace 'target_lang: English', "target_lang: $TargetLang"
$schemaContent | Set-Content -Path (Join-Path $RimeUserDir "typeanything.schema.yaml") -Encoding UTF8

# Seed default lang
Set-Content -Path (Join-Path $RimeUserDir "typeanything_lang.txt") -Value "en" -Encoding ASCII -NoNewline

Write-Host "==> Patch default.custom.yaml: only typeanything"
$customContent = @"
patch:
  schema_list:
    - schema: typeanything
"@
$customContent | Set-Content -Path (Join-Path $RimeUserDir "default.custom.yaml") -Encoding UTF8

Write-Host "==> Rebrand TSF profile descriptions to TypeAnything"
$root = "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$WeaselClsid\LanguageProfile"
if (Test-Path $root) {
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
        $profKeyPath = Join-Path $_.PSPath $ProfileGuid
        if (Test-Path $profKeyPath) {
            Set-ItemProperty -Path $profKeyPath -Name "Description" -Value "TypeAnything" -Type String -ErrorAction SilentlyContinue
        }
    }
}

Write-Host "==> Redeploy schema (poll until typeanything.prism.bin written)"
$buildCache = Join-Path $RimeUserDir "build"
if (Test-Path $buildCache) {
    Remove-Item -Recurse -Force $buildCache -ErrorAction SilentlyContinue
}
# WeaselDeployer is a GUI app: /deploy compiles the schema then enters a Win32
# message loop and never exits on its own. Detect compile completion by polling
# the build artifact's mtime, then force-kill the deployer immediately.
$markerFile = Join-Path $RimeUserDir "build\typeanything.prism.bin"
$startedAt  = Get-Date
$dep = Start-Process -WindowStyle Hidden -PassThru `
    -FilePath (Join-Path $WeaselDir "WeaselDeployer.exe") `
    -ArgumentList "/deploy"
$compiledIn = $null
while (-not $dep.HasExited) {
    if ((Test-Path $markerFile) -and ((Get-Item $markerFile).LastWriteTime -gt $startedAt)) {
        $compiledIn = (Get-Date) - $startedAt
        & cmd.exe /c "taskkill /F /T /PID $($dep.Id) 1>nul 2>nul" | Out-Null
        break
    }
    if (((Get-Date) - $startedAt).TotalSeconds -gt 60) {
        Write-Warning "    deployer hung > 60s with no compile output — force-killing. Check $RimeUserDir for schema errors."
        & cmd.exe /c "taskkill /F /T /PID $($dep.Id) 1>nul 2>nul" | Out-Null
        break
    }
    Start-Sleep -Milliseconds 300
}
if ($compiledIn) {
    Write-Host ("    schema compiled in {0:N1}s — deployer terminated" -f $compiledIn.TotalSeconds)
} elseif ($dep.HasExited) {
    Write-Host "    deployer exited code $($dep.ExitCode)"
}
& cmd.exe /c "taskkill /F /IM WeaselDeployer.exe /T 1>nul 2>nul" | Out-Null
Start-Sleep -Milliseconds 300
Start-Process (Join-Path $WeaselDir "WeaselServer.exe")

# TSF picks up new Description on next session/login or when language bar is
# refreshed. We avoid restarting explorer (causes the visible black flash and
# spurious folder windows). User can sign out + back in to see new name in the
# Win+Space menu, OR just wait — new apps already see TypeAnything.

if ($script:RebootNeeded) {
    Write-Host ""
    Write-Host "  >>> One or more binaries were in use; pending replacement on next reboot."
    Write-Host "  >>> Reboot now (or sign out + sign in) to finish the install."
} else {
    Write-Host ""
    Write-Host "  All binaries deployed cleanly."
}
Write-Host "Done."
