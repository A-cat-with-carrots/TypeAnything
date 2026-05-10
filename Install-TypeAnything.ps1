#requires -RunAsAdministrator
<#
TypeAnything one-click installer — wraps install-typeanything-to-weasel.ps1 with:
  1. Auto-detects whether prebuilt binaries ship next to this script
     (release ZIP layout) or whether the user is running from the source tree.
  2. GUI prompt for DeepSeek API key (no command-line typing).
  3. GUI prompt for default target language.
  4. Verifies Weasel is installed; if not, opens the Weasel download page.

Layouts supported:
  * Release ZIP:
      Install-TypeAnything.bat
      Install-TypeAnything.ps1                  (this file)
      install-typeanything-to-weasel.ps1
      binaries/
        rime.dll
        weaselx64.dll
        WeaselServer.exe
        WeaselDeployer.exe
      schema/
        typeanything.schema.yaml
  * Source tree:
      install-typeanything-to-weasel.ps1
      third_party/weasel/librime/dist/lib/rime.dll
      third_party/weasel/build/windows/x64/release/...
      third_party/weasel/librime/plugins/typeanything/schema/typeanything.schema.yaml
#>

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName Microsoft.VisualBasic

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$WeaselDir = "C:\Program Files\Rime\weasel-0.17.4"

# 1. Verify Weasel is installed.
if (-not (Test-Path $WeaselDir)) {
    [System.Windows.Forms.MessageBox]::Show(
        "Weasel is not installed at $WeaselDir.`n`nThe TypeAnything installer needs Weasel 0.17.4 first. Click OK to open the Weasel download page.",
        "TypeAnything Installer",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Warning) | Out-Null
    Start-Process "https://rime.im/download/"
    exit 1
}

# 2. Detect bundle layout.
$prebuiltDir = Join-Path $here "binaries"
$schemaPrebuilt = Join-Path $here "schema\typeanything.schema.yaml"
$useBundle = (Test-Path (Join-Path $prebuiltDir "rime.dll")) `
         -and (Test-Path (Join-Path $prebuiltDir "weaselx64.dll")) `
         -and (Test-Path (Join-Path $prebuiltDir "WeaselServer.exe")) `
         -and (Test-Path (Join-Path $prebuiltDir "WeaselDeployer.exe")) `
         -and (Test-Path $schemaPrebuilt)

if ($useBundle) {
    Write-Host "==> Detected bundled prebuilt binaries — using release layout."
    $rimeDll  = Join-Path $prebuiltDir "rime.dll"
    $schemaSrc = $schemaPrebuilt
    $buildBin = $prebuiltDir
} else {
    Write-Host "==> Using source-tree layout."
    $buildRoot = Join-Path $here "third_party\weasel"
    $rimeDll   = Join-Path $buildRoot "librime\dist\lib\rime.dll"
    $schemaSrc = Join-Path $buildRoot "librime\plugins\typeanything\schema\typeanything.schema.yaml"
    $buildBin  = Join-Path $buildRoot "build\windows\x64\release"
    if (-not (Test-Path $rimeDll)) {
        [System.Windows.Forms.MessageBox]::Show(
            "Cannot find prebuilt binaries OR a built source tree.`n`nExpected one of:`n  $prebuiltDir\rime.dll`n  $rimeDll`n`nPlease re-download the release ZIP, or build from source first.",
            "TypeAnything Installer",
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
        exit 1
    }
}

# 3. Prompt for DeepSeek API key (try to recover from existing schema first).
$existingSchema = Join-Path $env:APPDATA "Rime\typeanything.schema.yaml"
$defaultKey = ""
if (Test-Path $existingSchema) {
    $content = Get-Content $existingSchema -Raw -Encoding UTF8
    if ($content -match 'api_key:\s*"([^"]+)"') { $defaultKey = $Matches[1] }
}

$apiKey = [Microsoft.VisualBasic.Interaction]::InputBox(
    "Paste your DeepSeek API key (starts with 'sk-').`n`nGet one at https://platform.deepseek.com/`n`nThis will be saved to your Rime config so you only enter it once.",
    "TypeAnything Installer — Step 1 of 2",
    $defaultKey)

if ([string]::IsNullOrWhiteSpace($apiKey)) {
    [System.Windows.Forms.MessageBox]::Show(
        "API key is required. Install cancelled.",
        "TypeAnything Installer",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
    exit 1
}
if ($apiKey -notmatch '^sk-') {
    $confirm = [System.Windows.Forms.MessageBox]::Show(
        "That doesn't look like a DeepSeek API key (no 'sk-' prefix). Continue anyway?",
        "TypeAnything Installer",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Question)
    if ($confirm -ne [System.Windows.Forms.DialogResult]::Yes) { exit 1 }
}

# 4. Prompt for default target language.
$lang = [Microsoft.VisualBasic.Interaction]::InputBox(
    "Default target language. You can change this anytime from the tray icon (Switch Language menu).`n`nExamples: English, Japanese, Korean, French, German, Spanish, Cantonese, ...",
    "TypeAnything Installer — Step 2 of 2",
    "English")
if ([string]::IsNullOrWhiteSpace($lang)) { $lang = "English" }

# 5. Run the deployer.
Write-Host ""
Write-Host "==> Running deployer with bundle=$useBundle ..."
Write-Host ""

if ($useBundle) {
    # Inline the install logic with bundle paths so we don't need the long
    # parameterized script. (The repo install-typeanything-to-weasel.ps1 is
    # source-tree shaped; for bundles we re-implement the few steps inline.)
    & "$here\install-typeanything-to-weasel.ps1" `
        -ApiKey $apiKey `
        -WeaselDir $WeaselDir `
        -BuildRoot $here `
        -OurRimeDll $rimeDll `
        -SchemaSrc $schemaSrc `
        -TargetLang $lang `
        -BundleBinariesDir $buildBin
} else {
    & "$here\install-typeanything-to-weasel.ps1" `
        -ApiKey $apiKey `
        -TargetLang $lang
}

if ($LASTEXITCODE -ne 0) {
    [System.Windows.Forms.MessageBox]::Show(
        "Installer reported a non-zero exit ($LASTEXITCODE). Scroll the console window for details.",
        "TypeAnything Installer",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    exit $LASTEXITCODE
}

[System.Windows.Forms.MessageBox]::Show(
    "TypeAnything is installed.`n`nNext steps:`n  1. Sign out + sign back in (or reboot) to activate the new TSF DLL across all apps.`n  2. Press Win+Space and switch to TypeAnything.`n  3. Type pinyin, press Space to choose Chinese, then Enter to translate.",
    "TypeAnything Installer — Done",
    [System.Windows.Forms.MessageBoxButtons]::OK,
    [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
