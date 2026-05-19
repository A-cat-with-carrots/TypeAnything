#include "stdafx.h"
#include "WeaselServerApp.h"
#include <WeaselUtility.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <Shlobj.h>
#include <KnownFolders.h>
#include <thread>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

WeaselServerApp::WeaselServerApp()
    : m_handler(std::make_unique<RimeWithWeaselHandler>(&m_ui)),
      tray_icon(m_ui) {
  m_server.SetRequestHandler(m_handler.get());
  SetupMenuHandlers();
}

WeaselServerApp::~WeaselServerApp() {}

// ─── TypeAnything in-app update check ────────────────────────────────
//
// Fallback version string. Runtime preferred source = `DisplayVersion`
// value under HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\
// TypeAnything (written by ta-installer on every install). See
// GetInstalledVersion() below — this literal only kicks in when the
// registry read fails (e.g. running from a dev build that never went
// through the installer).
#ifndef TA_VERSION
#define TA_VERSION L"v0.6.4"
#endif

namespace {

// Read the installed version from the Add/Remove Programs registry entry
// (set by ta-installer step 12). Returns the literal e.g. "v0.6.4", with
// the "v" prefix prepended. Falls back to the compile-time TA_VERSION.
static std::wstring GetInstalledVersion() {
  HKEY h;
  if (RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TypeAnything",
          0, KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
    WCHAR buf[64] = {0};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExW(h, L"DisplayVersion", nullptr, &type,
                               (LPBYTE)buf, &sz);
    RegCloseKey(h);
    if (r == ERROR_SUCCESS && type == REG_SZ && buf[0]) {
      std::wstring v = L"v";
      v += buf;
      return v;
    }
  }
  return std::wstring(TA_VERSION);
}

// Minimal WinHTTP GET helper. Returns response body as UTF-8 string,
// or empty on any failure. Supports HTTPS only.
std::string HttpsGet(const std::wstring& host,
                     const std::wstring& path,
                     const wchar_t* user_agent,
                     DWORD timeout_ms = 12000) {
  HINTERNET hs = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) return "";
  WinHttpSetTimeouts(hs, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET hc = WinHttpConnect(hs, host.c_str(),
                                INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hc) { WinHttpCloseHandle(hs); return ""; }
  HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE);
  if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); return ""; }
  // GitHub API requires a User-Agent header (set via WinHttpOpen) and
  // accepts an Accept header for the v3 JSON content type.
  WinHttpAddRequestHeaders(hr, L"Accept: application/vnd.github+json",
                           (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
  std::string result;
  if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hr, nullptr)) {
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
      std::vector<char> buf(avail);
      DWORD read = 0;
      if (!WinHttpReadData(hr, buf.data(), avail, &read)) break;
      result.append(buf.data(), read);
    }
  }
  WinHttpCloseHandle(hr);
  WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  return result;
}

// Streaming WinHTTP GET to a file. Returns true on success.
bool HttpsGetToFile(const std::wstring& host,
                    const std::wstring& path,
                    const fs::path& out,
                    const wchar_t* user_agent,
                    DWORD timeout_ms = 60000) {
  HINTERNET hs = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) return false;
  WinHttpSetTimeouts(hs, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET hc = WinHttpConnect(hs, host.c_str(),
                                INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hc) { WinHttpCloseHandle(hs); return false; }
  HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE);
  if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); return false; }
  // GitHub release asset URLs return 302 to a CDN; follow redirects.
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(hr, WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                   sizeof(redirect));
  bool ok = false;
  if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hr, nullptr)) {
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (f) {
      DWORD avail = 0;
      ok = true;
      while (ok && WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hr, buf.data(), avail, &read)) { ok = false; break; }
        f.write(buf.data(), read);
        if (!f) { ok = false; break; }
      }
    }
  }
  WinHttpCloseHandle(hr);
  WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  return ok;
}

// Hand-rolled extract of a JSON string field. Looks for "<key>"\s*:\s*"...".
// Good enough for GitHub's release JSON shape; avoids pulling in a JSON
// dependency for this one feature.
std::string JsonStringField(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t i = json.find(needle);
  if (i == std::string::npos) return "";
  i += needle.size();
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t' ||
                             json[i] == ':' || json[i] == '\n' ||
                             json[i] == '\r'))
    ++i;
  if (i >= json.size() || json[i] != '"') return "";
  ++i;
  std::string out;
  while (i < json.size() && json[i] != '"') {
    if (json[i] == '\\' && i + 1 < json.size()) {
      char n = json[i + 1];
      if (n == 'n') out.push_back('\n');
      else if (n == 'r') out.push_back('\r');
      else if (n == 't') out.push_back('\t');
      else out.push_back(n);
      i += 2;
    } else {
      out.push_back(json[i++]);
    }
  }
  return out;
}

// Crude semver compare. Drops a leading "v" then compares numeric
// segments left-to-right. Returns >0 if a>b, <0 if a<b, 0 if equal.
int CompareVersion(const std::string& a, const std::string& b) {
  auto strip = [](const std::string& s) {
    return (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? s.substr(1) : s;
  };
  std::string sa = strip(a), sb = strip(b);
  size_t i = 0, j = 0;
  while (i < sa.size() || j < sb.size()) {
    int na = 0, nb = 0;
    while (i < sa.size() && isdigit((unsigned char)sa[i])) {
      na = na * 10 + (sa[i++] - '0');
    }
    while (j < sb.size() && isdigit((unsigned char)sb[j])) {
      nb = nb * 10 + (sb[j++] - '0');
    }
    if (na != nb) return na - nb;
    if (i < sa.size() && sa[i] == '.') ++i;
    if (j < sb.size() && sb[j] == '.') ++j;
  }
  return 0;
}

}  // namespace

namespace {
// All the work — network I/O + modal MessageBox + download +
// ShellExecute — happens on a worker thread so the tray menu handler
// (which runs on WeaselServer's main thread alongside IPC servicing for
// every running app's TSF) returns instantly. Otherwise typing freezes
// in every host process for the duration of the HTTP call and dialog.
// Return value is unused (the thread is detached); kept as bool only to
// avoid touching every `return true;` site below.
bool check_update_worker() {
  const wchar_t* UA = L"TypeAnything-Updater/1.0";
  const wchar_t* HOST = L"api.github.com";
  const wchar_t* PATH =
      L"/repos/A-cat-with-carrots/TypeAnything/releases/latest";

  std::string body = HttpsGet(HOST, PATH, UA);
  if (body.empty()) {
    MessageBoxW(NULL,
                L"无法连接 GitHub。请检查网络后重试。",
                L"TypeAnything 更新检查",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return true;
  }

  std::string tag = JsonStringField(body, "tag_name");
  std::string dl_url = JsonStringField(body, "browser_download_url");
  std::string release_name = JsonStringField(body, "name");
  if (tag.empty() || dl_url.empty()) {
    MessageBoxW(NULL,
                L"无法解析 GitHub 返回的版本信息。",
                L"TypeAnything 更新检查",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return true;
  }

  // Read current installed version from the registry (written by
  // ta-installer). Compare against the GitHub release tag.
  std::wstring wcur = GetInstalledVersion();
  std::string cur;
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, wcur.data(), (int)wcur.size(),
                                nullptr, 0, nullptr, nullptr);
    cur.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, wcur.data(), (int)wcur.size(),
                        cur.data(), n, nullptr, nullptr);
  }

  if (CompareVersion(tag, cur) <= 0) {
    std::wstring msg = L"已是最新版本（" + wcur + L"）。";
    MessageBoxW(NULL, msg.c_str(), L"TypeAnything 更新检查",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    return true;
  }

  // Build prompt text: latest tag + release name (if any).
  auto toWide = [](const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        w.data(), n);
    return w;
  };
  std::wstring prompt =
      L"发现新版本：" + toWide(tag) +
      (release_name.empty() ? L"" : (L"  " + toWide(release_name))) +
      L"\n\n当前版本：" + wcur + L"\n\n立即下载并安装吗？";
  int r = MessageBoxW(NULL, prompt.c_str(),
                      L"TypeAnything 更新可用",
                      MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
  if (r != IDYES) return true;

  // Parse host + path from dl_url. Format:
  //   https://github.com/<owner>/<repo>/releases/download/<tag>/<file>
  std::wstring wurl = toWide(dl_url);
  const std::wstring scheme = L"https://";
  if (wurl.compare(0, scheme.size(), scheme) != 0) {
    MessageBoxW(NULL, L"下载链接格式异常。", L"TypeAnything 更新",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return true;
  }
  size_t host_start = scheme.size();
  size_t path_start = wurl.find(L'/', host_start);
  if (path_start == std::wstring::npos) {
    MessageBoxW(NULL, L"下载链接格式异常。", L"TypeAnything 更新",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return true;
  }
  std::wstring dl_host = wurl.substr(host_start, path_start - host_start);
  std::wstring dl_path = wurl.substr(path_start);
  std::wstring file_name = dl_path.substr(dl_path.find_last_of(L'/') + 1);
  if (file_name.empty()) file_name = L"TypeAnything-installer.exe";

  // Download to %TEMP%.
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  fs::path dst = fs::path(tmp) / file_name;
  bool ok = HttpsGetToFile(dl_host, dl_path, dst, UA);
  if (!ok) {
    std::wstring err = L"下载失败。可以手动从浏览器打开下载页吗？\n\n"
                       + wurl;
    if (MessageBoxW(NULL, err.c_str(), L"TypeAnything 更新",
                    MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
      ShellExecuteW(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    return true;
  }

  // Launch installer. It will request UAC, kill running WeaselServer,
  // replace binaries, restart server.
  ShellExecuteW(NULL, L"open", dst.c_str(), NULL, NULL, SW_SHOWNORMAL);
  return true;
}
}  // namespace

bool WeaselServerApp::check_update() {
  // Fire-and-forget. The worker does its own UI; we don't await it.
  std::thread([]() { check_update_worker(); }).detach();
  return true;
}

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

// NOTE: the old PowerShell-InputBox language picker (show_lang_picker) was
// removed — the tray menu now opens ta-settings.exe --page lang instead
// (see SetupMenuHandlers / ID_WEASELTRAY_SWITCH_LANG below).

}  // namespace

void WeaselServerApp::SetupMenuHandlers() {
  std::filesystem::path dir = install_dir();

  m_server.AddMenuHandler(ID_WEASELTRAY_QUIT,
                          [this] { return m_server.Stop() == 0; });

  // Switch Language — open ta-settings.exe WebView2 UI
  m_server.AddMenuHandler(ID_WEASELTRAY_SWITCH_LANG, [dir] {
    std::wstring exe = (dir / L"ta-settings.exe").wstring();
    return (uintptr_t)ShellExecuteW(NULL, L"open", exe.c_str(),
                                    L"--page lang",
                                    dir.wstring().c_str(),
                                    SW_SHOWNORMAL) > 32;
  });

  // Check for updates
  m_server.AddMenuHandler(ID_WEASELTRAY_CHECKUPDATE, check_update);

  // Model config — open ta-settings.exe WebView2 UI on the model page
  m_server.AddMenuHandler(ID_WEASELTRAY_MODEL_CONFIG, [dir] {
    std::wstring exe = (dir / L"ta-settings.exe").wstring();
    return (uintptr_t)ShellExecuteW(NULL, L"open", exe.c_str(),
                                    L"--page model",
                                    dir.wstring().c_str(),
                                    SW_SHOWNORMAL) > 32;
  });

  // Restart — re-deploy schemas (also rebuilds dictionaries)
  m_server.AddMenuHandler(ID_WEASELTRAY_DEPLOY,
                          std::bind(execute, dir / L"WeaselDeployer.exe",
                                    std::wstring(L"/deploy")));
}
