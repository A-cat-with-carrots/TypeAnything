# Stage all artifacts for installer.rc to embed.
# Run before xmake build.

$ErrorActionPreference = "Stop"
$here = "D:\hrdai\aiForType\tools\ta-installer"
$embed = Join-Path $here "embed"

if (Test-Path $embed) { Remove-Item -Recurse -Force $embed }
New-Item -ItemType Directory -Force -Path $embed,"$embed\target-ui","$embed\install-ui" | Out-Null

# 1. Weasel binaries
$buildBin = "D:\hrdai\aiForType\third_party\weasel\build\windows\x64\release"
Copy-Item "D:\hrdai\aiForType\third_party\weasel\librime\dist\lib\rime.dll" "$embed\rime.dll"
Copy-Item "$buildBin\WeaselTSF\weaselx64.dll"           "$embed\weaselx64.dll"
Copy-Item "$buildBin\WeaselServer\WeaselServer.exe"     "$embed\WeaselServer.exe"
Copy-Item "$buildBin\WeaselDeployer\WeaselDeployer.exe" "$embed\WeaselDeployer.exe"

# 2. ta-settings.exe + loader
$taBin = "D:\hrdai\aiForType\tools\ta-settings\build\windows\x64\release"
Copy-Item "$taBin\ta-settings.exe"     "$embed\ta-settings.exe"
Copy-Item "$taBin\WebView2Loader.dll"  "$embed\WebView2Loader.dll"

# 3. target ui (for ta-settings to load post-install)
foreach ($f in "index.html","style.css","app.js","fish.png") {
    Copy-Item "$taBin\ui\$f" "$embed\target-ui\$f"
}

# 4. installer ui (this exe's own webview pages)
foreach ($f in "index.html","style.css","app.js","fish.png") {
    Copy-Item "$here\ui\$f" "$embed\install-ui\$f"
}

# 5. schema yaml template + supplement dict (modern AI/IT/slang terms)
Copy-Item "D:\hrdai\aiForType\third_party\weasel\librime\plugins\typeanything\schema\typeanything.schema.yaml" `
          "$embed\typeanything.schema.yaml"
Copy-Item "D:\hrdai\aiForType\third_party\weasel\librime\plugins\typeanything\schema\typeanything.dict.yaml" `
          "$embed\typeanything.dict.yaml"

# Compile installer.rc → .res → .obj (real COFF) so xmake link picks it up
# as a normal object file. xmake's built-in RC handling outputs RES format
# but under a .obj name, which link.exe silently drops.
$rcExe = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | ForEach-Object {
             $candidate = Join-Path $_.FullName "x64\rc.exe"
             if (Test-Path $candidate) { return $candidate }
         } | Select-Object -First 1
$cvtres = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | ForEach-Object {
              $candidate = Join-Path $_.FullName "bin\HostX64\x64\cvtres.exe"
              if (Test-Path $candidate) { return $candidate }
          } | Select-Object -First 1

if (-not $rcExe)   { throw "rc.exe not found (need Windows SDK)" }
if (-not $cvtres)  { throw "cvtres.exe not found (need VS2022 BuildTools)" }

Write-Host "==> rc.exe : $rcExe"
Write-Host "==> cvtres : $cvtres"

$rc  = Join-Path $here "installer.rc"
$res = Join-Path $here "embed\installer.res"
$obj = Join-Path $here "embed\installer.res.obj"

& $rcExe -nologo -fo $res $rc
if ($LASTEXITCODE -ne 0) { throw "rc.exe failed: $LASTEXITCODE" }
& $cvtres /MACHINE:X64 /NOLOGO /OUT:$obj $res
if ($LASTEXITCODE -ne 0) { throw "cvtres failed: $LASTEXITCODE" }

Write-Host "==> embed staged:"
Get-ChildItem -Recurse $embed | ForEach-Object { Write-Host ("  {0,10}B  {1}" -f $_.Length, $_.FullName.Substring($embed.Length + 1)) }
