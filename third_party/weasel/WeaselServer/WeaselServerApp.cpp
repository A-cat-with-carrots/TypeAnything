#include "stdafx.h"
#include "WeaselServerApp.h"
#include <WeaselUtility.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <Shlobj.h>
#include <KnownFolders.h>

WeaselServerApp::WeaselServerApp()
    : m_handler(std::make_unique<RimeWithWeaselHandler>(&m_ui)),
      tray_icon(m_ui) {
  m_server.SetRequestHandler(m_handler.get());
  SetupMenuHandlers();
}

WeaselServerApp::~WeaselServerApp() {}

int WeaselServerApp::Run() {
  if (!m_server.Start())
    return -1;

  // TypeAnything: WinSparkle disabled. Upstream Weasel appcast at
  // rime.github.io/release/weasel/appcast.xml triggers a startup dialog
  // that confuses users into reinstalling the original Weasel. We use
  // ShellExecute to GitHub Releases for manual update checks instead.
m_ui.Create(m_server.GetHWnd());

  m_handler->Initialize();
  m_handler->OnUpdateUI([this]() { tray_icon.Refresh(); });

  tray_icon.Create(m_server.GetHWnd());
  tray_icon.Refresh();

  int ret = m_server.Run();

  m_handler->Finalize();
  m_ui.Destroy();
  tray_icon.RemoveIcon();
  // win_sparkle_cleanup() removed (TypeAnything: WinSparkle disabled)

  return ret;
}

namespace {
// Languages exposed in the tray "Switch Language" submenu. Code -> display name.
struct LangEntry {
  const wchar_t* display;
  const char* code;
};
static const LangEntry kLangs[] = {
    {L"English",       "en"},
    {L"日本語",        "ja"},
    {L"한국어",         "ko"},
    {L"粵語 / Cantonese","yue"},
    {L"Français",      "fr"},
    {L"Deutsch",       "de"},
    {L"Español",       "es"},
    {L"Italiano",      "it"},
    {L"Português",     "pt"},
    {L"Русский",       "ru"},
    {L"العربية",       "ar"},
    {L"Tiếng Việt",    "vi"},
    {L"ไทย",            "th"},
    {L"Bahasa Indonesia","id"},
    {L"Türkçe",        "tr"},
    {L"हिन्दी",          "hi"},
    {L"Tiếng Việt cổ", "vi"},
    {L"Nederlands",    "nl"},
    {L"Polski",        "pl"},
    {L"Svenska",       "sv"},
};

std::filesystem::path lang_file_path() {
  // %APPDATA%\Rime\typeanything_lang.txt
  PWSTR path_str = nullptr;
  std::filesystem::path p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL,
                                     &path_str))) {
    p = std::filesystem::path(path_str) / L"Rime" /
        L"typeanything_lang.txt";
    CoTaskMemFree(path_str);
  } else {
    p = L"typeanything_lang.txt";
  }
  return p;
}

void write_lang(const char* code) {
  auto p = lang_file_path();
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::out | std::ios::trunc);
  if (f) f << code;
}

void show_lang_picker(HWND /*hwnd*/) {
  // TypeAnything: free-form natural-language target picker via PS InputBox.
  // User types anything ("English" / "学术英语" / "中二日语" / "Klingon").
  std::filesystem::path lang_path = lang_file_path();
  std::wstring lang_file = lang_path.wstring();

  std::wstring ps;
  ps += L"-NoProfile -WindowStyle Hidden -Command \"";
  ps += L"Add-Type -AssemblyName Microsoft.VisualBasic;";
  ps += L"$f='";
  for (wchar_t c : lang_file) {
    if (c == L'\'') ps += L"''";
    else ps += c;
  }
  ps += L"';";
  ps += L"$cur=if(Test-Path $f){";
  ps += L"((Get-Content -LiteralPath $f -Encoding UTF8 -Raw)";
  ps += L" -split '`r?`n' | Where-Object {$_ -and $_ -notmatch '^\s*#'} | Select-Object -First 1)";
  ps += L"}else{'English'};";
  ps += L"if($null -eq $cur){$cur='English'};";
  ps += L"$msg=\"切换翻译目标 ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` ` `n";
  ps += L"输入任意自然语言描述，DeepSeek 会按描述翻译。`n`n";
  ps += L"示例：`n";
  ps += L"  English / 日本語 / 한국어 / Français / 粵語`n";
  ps += L"  学术英语 / 商务日语 / 中二风格的日语`n";
  ps += L"  Klingon battle prose / Spanish chilango`n";
  ps += L"  古汉语风格 / 网络流行语\";";
  ps += L"$r=[Microsoft.VisualBasic.Interaction]::InputBox($msg,'TypeAnything 切换语言',$cur);";
  ps += L"if($r){[System.IO.File]::WriteAllText($f,$r,";
  ps += L"(New-Object System.Text.UTF8Encoding $false))}\"";

  ShellExecuteW(NULL, L"open", L"powershell.exe", ps.c_str(),
                NULL, SW_SHOWNORMAL);
}

}  // namespace

void WeaselServerApp::SetupMenuHandlers() {
  std::filesystem::path dir = install_dir();

  m_server.AddMenuHandler(ID_WEASELTRAY_QUIT,
                          [this] { return m_server.Stop() == 0; });

  // Switch Language — pop a submenu with all languages
  m_server.AddMenuHandler(ID_WEASELTRAY_SWITCH_LANG, [this] {
    show_lang_picker(m_server.GetHWnd());
    return true;
  });

  // Check for updates
  m_server.AddMenuHandler(ID_WEASELTRAY_CHECKUPDATE, check_update);

  // Restart — re-deploy schemas (also rebuilds dictionaries)
  m_server.AddMenuHandler(ID_WEASELTRAY_DEPLOY,
                          std::bind(execute, dir / L"WeaselDeployer.exe",
                                    std::wstring(L"/deploy")));
}
