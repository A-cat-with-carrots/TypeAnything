# Rebrand: change all "Description" entries under Weasel TSF profile to "TypeAnything"
# + change tooltip strings inside HKCU. Restart explorer at end so language bar reloads.
#
# Run as admin.

#requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

$WeaselClsid = "{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}"
$ProfileGuid = "{3D02CAB6-2B8E-4781-BA20-1C9267529467}"
$NewName = "TypeAnything"

$root = "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$WeaselClsid\LanguageProfile"

if (-not (Test-Path $root)) {
    Write-Host "WARN: $root not found. Is Weasel installed?"
    exit 1
}

$count = 0
Get-ChildItem $root | ForEach-Object {
    $langKey = $_
    $profKeyPath = Join-Path $langKey.PSPath $ProfileGuid
    if (Test-Path $profKeyPath) {
        Set-ItemProperty -Path $profKeyPath -Name "Description" -Value $NewName -Type String
        Write-Host "  patched $($langKey.PSChildName) -> Description=$NewName"
        $count++
    }
}

Write-Host "Patched $count language profiles."

# HKCU per-user assoc may also keep cached desc; touching it forces refresh on next session.
$hkcuRoot = "HKCU:\SOFTWARE\Microsoft\CTF\TIP\$WeaselClsid\LanguageProfile"
if (Test-Path $hkcuRoot) {
    Get-ChildItem $hkcuRoot -Recurse | ForEach-Object {
        try {
            $p = Get-ItemProperty -Path $_.PSPath -ErrorAction SilentlyContinue
            if ($p.Description) {
                Set-ItemProperty -Path $_.PSPath -Name "Description" -Value $NewName -Type String -ErrorAction SilentlyContinue
                Write-Host "  patched HKCU $($_.PSChildName) -> Description=$NewName"
            }
        } catch {}
    }
}

# Restart explorer to refresh language bar / taskbar IME UI
Write-Host ""
Write-Host "Restarting explorer.exe to refresh language bar..."
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
Start-Process explorer.exe
Write-Host ""
Write-Host "Done. Sign out + sign in to Windows for full refresh if name still shows old."
