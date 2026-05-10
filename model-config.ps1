# TypeAnything 模型配置 — WinForms popup that lets the user point the IME at any
# OpenAI Chat Completions compatible endpoint (DeepSeek default / OpenAI / Moonshot
# / 智谱 / 自托管 vLLM / Ollama). Saves to %APPDATA%\Rime\typeanything.schema.yaml
# and forces a schema redeploy + WeaselServer restart so the new config takes
# effect immediately.
#
# Spawned by:
#   * Tray icon menu "模型配置 (M)"  (WeaselServer)
#   * TSF language bar menu "模型配置 (M)"  (weaselx64.dll, in client process)
#
# Both spawn this script via:
#   ShellExecuteW(NULL, L"open", L"powershell.exe",
#                 L"-NoProfile -STA -ExecutionPolicy Bypass -File <thisfile>",
#                 NULL, SW_SHOWNORMAL)

[CmdletBinding()]
param()

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$schemaFile = Join-Path $env:APPDATA "Rime\typeanything.schema.yaml"

if (-not (Test-Path $schemaFile)) {
    [System.Windows.Forms.MessageBox]::Show(
        "未找到 schema 配置文件：`n$schemaFile`n`n请先用 Install-TypeAnything 完成首次安装。",
        "TypeAnything 模型配置",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Warning) | Out-Null
    exit 1
}

# Read current values from schema yaml
$content = Get-Content -LiteralPath $schemaFile -Raw -Encoding UTF8

function Read-Field([string]$key, [string]$default) {
    $m = [regex]::Match($content, "(?m)^\s*$key`:\s*`"?([^`"\r\n]*)`"?\s*\$|(?m)^\s*$key`:\s*([^\r\n#]*?)\s*(?:#|$)")
    if ($m.Success) {
        for ($i = 1; $i -lt $m.Groups.Count; $i++) {
            $v = $m.Groups[$i].Value
            if (![string]::IsNullOrWhiteSpace($v)) { return $v.Trim().Trim('"') }
        }
    }
    return $default
}

$curApiKey = Read-Field "api_key" ""
$curModel  = Read-Field "model"   "deepseek-chat"
$curHost   = Read-Field "host"    "api.deepseek.com"
$curPath   = Read-Field "path"    "/v1/chat/completions"

# Build form
$form = New-Object System.Windows.Forms.Form
$form.Text = "TypeAnything 模型配置"
$form.Size = New-Object System.Drawing.Size(560, 420)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.Font = New-Object System.Drawing.Font("Microsoft YaHei UI", 9)

$intro = New-Object System.Windows.Forms.Label
$intro.Text = "你的输入只会发到下面这个 endpoint。无中转，无后端代理。`n默认填的是 DeepSeek，可改 OpenAI / Moonshot / 智谱 / 自托管 vLLM / Ollama 等任意 OpenAI Chat Completions 兼容 API。"
$intro.Location = New-Object System.Drawing.Point(15, 12)
$intro.Size = New-Object System.Drawing.Size(520, 38)
$intro.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($intro)

function Add-Row($label, $value, [int]$y, [bool]$mask = $false) {
    $lbl = New-Object System.Windows.Forms.Label
    $lbl.Text = $label
    $lbl.Location = New-Object System.Drawing.Point(15, ($y + 4))
    $lbl.Size = New-Object System.Drawing.Size(80, 20)
    $form.Controls.Add($lbl)

    $tb = New-Object System.Windows.Forms.TextBox
    $tb.Location = New-Object System.Drawing.Point(100, $y)
    $tb.Size = New-Object System.Drawing.Size(430, 24)
    $tb.Text = $value
    if ($mask) { $tb.UseSystemPasswordChar = $true }
    $form.Controls.Add($tb)

    return $tb
}

$tbApiKey = Add-Row "API Key" $curApiKey 60 $true
$tbModel  = Add-Row "Model"   $curModel  95
$tbHost   = Add-Row "Host"    $curHost   130
$tbPath   = Add-Row "Path"    $curPath   165

# Show/hide api key checkbox
$cbShow = New-Object System.Windows.Forms.CheckBox
$cbShow.Text = "显示 API Key"
$cbShow.Location = New-Object System.Drawing.Point(100, 88)
$cbShow.Size = New-Object System.Drawing.Size(120, 20)
$cbShow.Add_CheckedChanged({
    $tbApiKey.UseSystemPasswordChar = -not $cbShow.Checked
})
$form.Controls.Add($cbShow)

# Examples
$ex = New-Object System.Windows.Forms.Label
$ex.Text = "常见组合:" + [Environment]::NewLine + `
           "  DeepSeek :  host=api.deepseek.com   path=/v1/chat/completions   model=deepseek-chat" + [Environment]::NewLine + `
           "  OpenAI   :  host=api.openai.com     path=/v1/chat/completions   model=gpt-4o" + [Environment]::NewLine + `
           "  Moonshot :  host=api.moonshot.cn    path=/v1/chat/completions   model=moonshot-v1-8k" + [Environment]::NewLine + `
           "  Ollama   :  host=localhost:11434    path=/v1/chat/completions   model=qwen2.5:7b"
$ex.Location = New-Object System.Drawing.Point(15, 200)
$ex.Size = New-Object System.Drawing.Size(520, 90)
$ex.ForeColor = [System.Drawing.Color]::DarkSlateGray
$ex.Font = New-Object System.Drawing.Font("Consolas", 8.5)
$form.Controls.Add($ex)

# Get API key links
$lnk = New-Object System.Windows.Forms.LinkLabel
$lnk.Text = "前往 DeepSeek 注册 + 充值 + 拿 API key"
$lnk.Location = New-Object System.Drawing.Point(15, 295)
$lnk.Size = New-Object System.Drawing.Size(300, 18)
$lnk.Add_LinkClicked({
    Start-Process "https://platform.deepseek.com/"
})
$form.Controls.Add($lnk)

$btnSave = New-Object System.Windows.Forms.Button
$btnSave.Text = "保存并应用"
$btnSave.Size = New-Object System.Drawing.Size(120, 32)
$btnSave.Location = New-Object System.Drawing.Point(290, 330)
$btnSave.DialogResult = [System.Windows.Forms.DialogResult]::OK
$form.Controls.Add($btnSave)
$form.AcceptButton = $btnSave

$btnCancel = New-Object System.Windows.Forms.Button
$btnCancel.Text = "取消"
$btnCancel.Size = New-Object System.Drawing.Size(120, 32)
$btnCancel.Location = New-Object System.Drawing.Point(415, 330)
$btnCancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.Controls.Add($btnCancel)
$form.CancelButton = $btnCancel

$result = $form.ShowDialog()
if ($result -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }

# Persist: regex-replace each field in schema yaml using MatchEvaluator so
# replacement string is literal (avoids $1/$$ substitution gotchas when API key
# contains weird chars).
function Update-Field([string]$src, [string]$key, [string]$newVal, [bool]$quoted) {
    $val = if ($quoted) { '"' + ($newVal -replace '"', '\"') + '"' } else { $newVal }
    $pat = "(?m)^(\s*" + [regex]::Escape($key) + ":\s*).*$"
    $rx  = [regex]$pat
    return $rx.Replace($src, [System.Text.RegularExpressions.MatchEvaluator]{
        param($m) $m.Groups[1].Value + $val
    }, 1)
}
$new = $content
$new = Update-Field $new "api_key" $tbApiKey.Text  $true
$new = Update-Field $new "model"   $tbModel.Text   $false
$new = Update-Field $new "host"    $tbHost.Text    $false
$new = Update-Field $new "path"    $tbPath.Text    $false

[System.IO.File]::WriteAllText($schemaFile, $new, (New-Object System.Text.UTF8Encoding $false))

# Trigger schema rebuild + server restart so changes take effect now.
$weaselDir = "C:\Program Files\Rime\weasel-0.17.4"
$deployer = Join-Path $weaselDir "WeaselDeployer.exe"
$server   = Join-Path $weaselDir "WeaselServer.exe"

# Kill running server so it re-loads with new config.
& cmd.exe /c "taskkill /F /IM WeaselServer.exe /T 1>nul 2>nul" | Out-Null
Start-Sleep -Milliseconds 500

# Run deployer to rebuild prism.bin (handles schema yaml updates).
if (Test-Path $deployer) {
    Start-Process -WindowStyle Hidden -FilePath $deployer -ArgumentList "/deploy"
    Start-Sleep -Seconds 3
    & cmd.exe /c "taskkill /F /IM WeaselDeployer.exe /T 1>nul 2>nul" | Out-Null
}
# Restart server (HKLM Run value points to it on next login; for now we start it).
if (Test-Path $server) {
    Start-Process -FilePath $server
}

[System.Windows.Forms.MessageBox]::Show(
    "已保存到 $schemaFile`n配置已生效（WeaselServer 已重启）。",
    "TypeAnything 模型配置",
    [System.Windows.Forms.MessageBoxButtons]::OK,
    [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
