# Build ta-settings.exe (WebView2 UI for TypeAnything tray menu items).

$ErrorActionPreference = "Stop"
Set-Location "D:\hrdai\products\TypeAnything\tools\ta-settings"

# Strip MSYS / mingw from PATH so xmake picks MSVC cl.exe
$pathParts = $env:PATH -split ';' | Where-Object {
    $_ -notmatch '\\Git\\(mingw|usr)' `
    -and $_ -notmatch '\\msys' `
    -and $_ -notmatch '\\Git\\bin' `
    -and $_ -notmatch '\\anaconda3'
}
$vsInstaller = "C:\Program Files (x86)\Microsoft Visual Studio\Installer"
if ((Test-Path $vsInstaller) -and ($pathParts -notcontains $vsInstaller)) {
    $pathParts = ,$vsInstaller + $pathParts
}
$env:PATH = ($pathParts -join ';')
$env:VSINSTALLERDIR = "$vsInstaller\"

# Activate VS env
$tmpEnvFile = Join-Path $env:TEMP "vcvars_env_ta.txt"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
cmd.exe /c "`"$vcvars`" x64 >nul 2>nul && set > `"$tmpEnvFile`""
foreach ($line in (Get-Content $tmpEnvFile -Encoding Default)) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
    }
}
$env:INCLUDE = ($env:INCLUDE -split ';' | Where-Object { $_ -notmatch '\\anaconda3' }) -join ';'
$env:LIB     = ($env:LIB     -split ';' | Where-Object { $_ -notmatch '\\anaconda3' }) -join ';'

# Configure xmake for x64 release
& xmake.exe f -p windows -a x64 -m release -y
if ($LASTEXITCODE -ne 0) { Write-Host "xmake config failed"; exit $LASTEXITCODE }

# Build
& xmake.exe build -j4 -y ta-settings
if ($LASTEXITCODE -ne 0) { Write-Host "xmake build failed"; exit $LASTEXITCODE }

Write-Host ""
Write-Host "=== ta-settings build done ==="
$out = ".\build\windows\x64\release"
Get-ChildItem -Recurse $out -Include *.exe,*.dll,*.html,*.css,*.js | ForEach-Object {
    Write-Host "  $($_.FullName)"
}
