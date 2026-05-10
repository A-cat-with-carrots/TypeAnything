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

  win_sparkle_set_registry_path("Software\\Rime\\Weasel\\Updates");
  if (GetThreadUILanguage() ==
      MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
    win_sparkle_set_lang("zh-TW");
  else if (GetThreadUILanguage() ==
           MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
    win_sparkle_set_lang("zh-CN");
  else
    win_sparkle_set_lang("en");
  win_sparkle_init();
  m_ui.Create(m_server.GetHWnd());

  m_handler->Initialize();
  m_handler->OnUpdateUI([this]() { tray_icon.Refresh(); });

  tray_icon.Create(m_server.GetHWnd());
  tray_icon.Refresh();

  int ret = m_server.Run();

  m_handler->Finalize();
  m_ui.Destroy();
  tray_icon.RemoveIcon();
  win_sparkle_cleanup();

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
  // %APPDATA%\Rime\typeeverything_lang.txt
  PWSTR path_str = nullptr;
  std::filesystem::path p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL,
                                     &path_str))) {
    p = std::filesystem::path(path_str) / L"Rime" /
        L"typeeverything_lang.txt";
    CoTaskMemFree(path_str);
  } else {
    p = L"typeeverything_lang.txt";
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

void show_lang_picker(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  for (size_t i = 0; i < sizeof(kLangs) / sizeof(kLangs[0]); ++i) {
    AppendMenuW(menu, MF_STRING,
                ID_WEASELTRAY_LANG_BASE + i, kLangs[i].display);
  }
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  UINT cmd = TrackPopupMenu(menu,
                            TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                            pt.x, pt.y, 0, hwnd, NULL);
  DestroyMenu(menu);
  if (cmd >= ID_WEASELTRAY_LANG_BASE &&
      cmd < ID_WEASELTRAY_LANG_BASE + sizeof(kLangs) / sizeof(kLangs[0])) {
    write_lang(kLangs[cmd - ID_WEASELTRAY_LANG_BASE].code);
  }
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
