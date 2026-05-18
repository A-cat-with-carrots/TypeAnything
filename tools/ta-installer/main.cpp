// TypeAnything one-click installer.
//
// Single-exe distribution that bundles every shipping artifact as RT_RCDATA
// (Weasel binaries + ta-settings.exe + WebView2Loader.dll + ui/ + schema
// yaml), shows a hrdai-styled WebView2 wizard, and performs the full deploy
// without ever spawning PowerShell:
//
//   1. UAC-elevated (via embedded manifest requireAdministrator).
//   2. Show webview UI; user enters API key + default target.
//   3. Worker thread: stop existing Weasel processes, extract embedded
//      artifacts to C:\Program Files\Rime\weasel-0.17.4\, write schema yaml
//      with injected api_key, patch HKLM\SOFTWARE\Microsoft\CTF\TIP profile
//      descriptions to "TypeAnything", run WeaselDeployer /deploy, start
//      WeaselServer.
//   4. Done screen: tells user new IME is usable in any *new* app without
//      reboot.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>   // IFileOpenDialog (folder picker)
#include <shlguid.h>    // CLSID_ShellLink, IID_IShellLinkW
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "../ta-settings/vendor/webview.h"

namespace fs = std::filesystem;

// ─── small helpers ──────────────────────────────────────────────

static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                              nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                      s.data(), n, nullptr, nullptr);
  return s;
}

static std::string JsonEscape(const std::string& s) {
  std::string r; r.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') { r.push_back('\\'); r.push_back(c); }
    else if (c == '\n') r += "\\n";
    else if (c == '\r') r += "\\r";
    else if (c == '\t') r += "\\t";
    else if ((unsigned char)c < 0x20) {
      char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
      r += buf;
    } else r.push_back(c);
  }
  return r;
}

static std::vector<std::string> ParseJsonStringArray(const std::string& json) {
  std::vector<std::string> out;
  size_t i = 0;
  auto skip = [&]() { while (i < json.size() && isspace((unsigned char)json[i])) ++i; };
  skip();
  if (i >= json.size() || json[i] != '[') return out;
  ++i;
  while (i < json.size()) {
    skip();
    if (json[i] == ']') break;
    if (json[i] != '"') break;
    ++i;
    std::string s;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) {
        char e = json[i + 1];
        switch (e) {
          case 'n': s.push_back('\n'); break;
          case 'r': s.push_back('\r'); break;
          case 't': s.push_back('\t'); break;
          case '"': s.push_back('"');  break;
          case '\\': s.push_back('\\'); break;
          case '/': s.push_back('/'); break;
          case 'u': {
            if (i + 5 < json.size()) {
              unsigned cp = 0;
              for (int k = 0; k < 4; ++k) {
                char h = json[i + 2 + k]; cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
              }
              i += 4;
              if (cp < 0x80) s.push_back((char)cp);
              else if (cp < 0x800) {
                s.push_back((char)(0xC0 | (cp >> 6)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
              } else {
                s.push_back((char)(0xE0 | (cp >> 12)));
                s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
              }
            }
            break;
          }
          default: s.push_back(e); break;
        }
        i += 2;
      } else {
        s.push_back(json[i]); ++i;
      }
    }
    out.push_back(std::move(s));
    if (i < json.size()) ++i;
    skip();
    if (i < json.size() && json[i] == ',') ++i;
  }
  return out;
}

// ─── resource extraction ────────────────────────────────────────

static std::pair<const void*, DWORD> LoadEmbedded(LPCWSTR name) {
  HRSRC h = FindResourceW(nullptr, name, RT_RCDATA);
  if (!h) return {nullptr, 0};
  DWORD sz = SizeofResource(nullptr, h);
  HGLOBAL g = LoadResource(nullptr, h);
  if (!g) return {nullptr, 0};
  return {LockResource(g), sz};
}

static bool WriteEmbeddedToFile(LPCWSTR res_name, const fs::path& dst) {
  auto [ptr, sz] = LoadEmbedded(res_name);
  if (!ptr) return false;
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  std::ofstream f(dst, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write((const char*)ptr, (std::streamsize)sz);
  return f.good();
}

// Same, but with MoveFileEx pending-on-reboot fallback when dst is locked.
static bool WriteEmbeddedToLockableFile(LPCWSTR res_name,
                                        const fs::path& dst,
                                        bool* reboot_needed) {
  auto [ptr, sz] = LoadEmbedded(res_name);
  if (!ptr) return false;
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);

  // Direct write retry.
  for (int i = 0; i < 8; ++i) {
    std::ofstream f(dst, std::ios::binary | std::ios::trunc);
    if (f) {
      f.write((const char*)ptr, (std::streamsize)sz);
      if (f.good()) return true;
    }
    Sleep(300);
  }

  // Pending-on-reboot fallback.
  fs::path tmp = dst.wstring() + L".new";
  std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write((const char*)ptr, (std::streamsize)sz);
  if (!f.good()) return false;
  f.close();
  if (MoveFileExW(tmp.c_str(), dst.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
    if (reboot_needed) *reboot_needed = true;
    return true;
  }
  return false;
}

// ─── filesystem ─────────────────────────────────────────────────

static fs::path WeaselDir() {
  // HKLM\Software\Rime\Weasel:WeaselRoot
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Rime\\Weasel", 0,
                    KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
    WCHAR buf[MAX_PATH] = {0}; DWORD len = sizeof(buf); DWORD type = 0;
    if (RegQueryValueExW(hKey, L"WeaselRoot", nullptr, &type,
                         (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ) {
      RegCloseKey(hKey); return fs::path(buf);
    }
    RegCloseKey(hKey);
  }
  return fs::path(L"C:\\Program Files\\Rime\\weasel-0.17.4");
}

// IFileOpenDialog folder picker. Returns the selected path (UTF-8) or
// "" if user cancelled / on error. Initial location: `start` if provided.
static std::string PickInstallDir(HWND parent, const std::wstring& start) {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg));
  if (FAILED(hr) || !dlg) return out;
  DWORD opt = 0;
  dlg->GetOptions(&opt);
  dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"选择 TypeAnything 安装位置");
  if (!start.empty()) {
    // Try to set initial folder. Best-effort: walk up until a path exists.
    std::filesystem::path p(start);
    std::error_code ec;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
      auto parent = p.parent_path();
      if (parent == p) break;
      p = parent;
    }
    if (!p.empty()) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), NULL,
                                                IID_PPV_ARGS(&item))) && item) {
        dlg->SetFolder(item);
        item->Release();
      }
    }
  }
  if (SUCCEEDED(dlg->Show(parent))) {
    IShellItem* res = nullptr;
    if (SUCCEEDED(dlg->GetResult(&res)) && res) {
      PWSTR path = nullptr;
      if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        out = WideToUtf8(path);
        CoTaskMemFree(path);
      }
      res->Release();
    }
  }
  dlg->Release();
  return out;
}

// Write HKLM\Software\Rime\Weasel\WeaselRoot = <install_dir>. Also writes
// the 32-bit redirected view so legacy / 32-bit consumers (WeaselSetup,
// any apps that read under WOW6432Node) see the same value.
static void WriteWeaselRootRegistry(const fs::path& install_dir) {
  std::wstring val = install_dir.wstring();
  REGSAM views[] = {KEY_WRITE | KEY_WOW64_64KEY, KEY_WRITE | KEY_WOW64_32KEY};
  for (REGSAM v : views) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Rime\\Weasel",
                        0, NULL, 0, v, NULL, &h, NULL) == ERROR_SUCCESS) {
      RegSetValueExW(h, L"WeaselRoot", 0, REG_SZ,
                     (const BYTE*)val.c_str(),
                     (DWORD)((val.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(h);
    }
  }
}

// Write Add/Remove Programs entry pointing at uninstall.exe in install dir.
static void WriteUninstallRegistry(const fs::path& install_dir,
                                   const std::wstring& version_str) {
  HKEY h;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TypeAnything",
                      0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL)
      != ERROR_SUCCESS) {
    return;
  }
  auto sz = [](HKEY h, const wchar_t* k, const std::wstring& v) {
    RegSetValueExW(h, k, 0, REG_SZ, (const BYTE*)v.c_str(),
                   (DWORD)((v.size() + 1) * sizeof(wchar_t)));
  };
  auto dw = [](HKEY h, const wchar_t* k, DWORD v) {
    RegSetValueExW(h, k, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
  };
  std::wstring uninst_exe = (install_dir / L"uninstall.exe").wstring();
  std::wstring quoted_cmd = L"\"" + uninst_exe + L"\" /uninstall";
  sz(h, L"DisplayName", L"TypeAnything 输入法");
  sz(h, L"DisplayVersion", version_str);
  sz(h, L"Publisher", L"HRDAI");
  sz(h, L"InstallLocation", install_dir.wstring());
  sz(h, L"UninstallString", quoted_cmd);
  sz(h, L"DisplayIcon", uninst_exe);
  sz(h, L"URLInfoAbout",
     L"https://github.com/A-cat-with-carrots/TypeAnything");
  dw(h, L"NoModify", 1);
  dw(h, L"NoRepair", 1);
  RegCloseKey(h);
}

static fs::path RimeUserDir() {
  PWSTR p = nullptr; std::wstring root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) {
    root = p; CoTaskMemFree(p);
  }
  return fs::path(root) / L"Rime";
}

// Base64 encode raw bytes. Used to inline fish.png into the HTML so we
// don't need to extract files for the installer's own webview UI.
static std::string Base64(const unsigned char* data, size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    unsigned v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >>  6) & 63]);
    out.push_back(tbl[ v        & 63]);
    i += 3;
  }
  if (i + 1 == len) {
    unsigned v = data[i] << 16;
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out += "==";
  } else if (i + 2 == len) {
    unsigned v = (data[i] << 16) | (data[i+1] << 8);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >>  6) & 63]);
    out += "=";
  }
  return out;
}

// Read an embedded resource into a std::string (for HTML/CSS/JS).
static std::string LoadEmbeddedAsString(LPCWSTR name) {
  auto [ptr, sz] = LoadEmbedded(name);
  if (!ptr || sz == 0) return "";
  return std::string((const char*)ptr, (size_t)sz);
}

// Build the all-in-one installer HTML by inlining CSS, JS, and the
// logo as base64. We then feed it to webview.set_html(), which avoids
// the file:// path resolution problems that the elevated UAC token
// occasionally introduces.
static std::string BuildInlineHtml() {
  std::string html = LoadEmbeddedAsString(MAKEINTRESOURCEW(IDR_INSTALL_HTML));
  std::string css  = LoadEmbeddedAsString(MAKEINTRESOURCEW(IDR_INSTALL_CSS));
  std::string js   = LoadEmbeddedAsString(MAKEINTRESOURCEW(IDR_INSTALL_JS));
  auto [png_ptr, png_sz] = LoadEmbedded(MAKEINTRESOURCEW(IDR_INSTALL_PNG));

  // Replace <link rel="stylesheet" href="style.css" />
  {
    std::string needle = R"(<link rel="stylesheet" href="style.css" />)";
    size_t pos = html.find(needle);
    if (pos != std::string::npos) {
      html.replace(pos, needle.size(), "<style>\n" + css + "\n</style>");
    }
  }
  // Replace <script src="app.js"></script>
  {
    std::string needle = R"(<script src="app.js"></script>)";
    size_t pos = html.find(needle);
    if (pos != std::string::npos) {
      html.replace(pos, needle.size(), "<script>\n" + js + "\n</script>");
    }
  }
  // Replace fish.png references with data: URL.
  if (png_ptr && png_sz > 0) {
    std::string b64 = Base64((const unsigned char*)png_ptr, (size_t)png_sz);
    std::string data_url = "data:image/png;base64," + b64;
    for (const std::string& ref : {
            std::string("href=\"fish.png\""),
            std::string("src=\"fish.png\""),
            std::string("url(\"fish.png\")"),
            std::string("url(fish.png)") }) {
      size_t pos = 0;
      while ((pos = html.find(ref, pos)) != std::string::npos) {
        std::string repl;
        if (ref.find("href") != std::string::npos)       repl = "href=\"" + data_url + "\"";
        else if (ref.find("src")  != std::string::npos)  repl = "src=\""  + data_url + "\"";
        else if (ref.find("url(\"") != std::string::npos) repl = "url(\"" + data_url + "\")";
        else                                              repl = "url("   + data_url + ")";
        html.replace(pos, ref.size(), repl);
        pos += repl.size();
      }
    }
  }
  return html;
}

// ─── process control ────────────────────────────────────────────

static void KillByName(const wchar_t* image) {
  std::wstring cmd = std::wstring(L"taskkill /F /IM ") + image + L" /T";
  STARTUPINFOW si{}; si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }
}

static void StartHidden(const fs::path& exe, const std::wstring& args = L"") {
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  if (!args.empty()) cmd += L" " + args;
  STARTUPINFOW si{}; si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }
}

static bool StartShown(const fs::path& exe) {
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                     0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
  }
  return false;
}

// ─── Cold-register TSF DLL ──────────────────────────────────────
//
// Standard Weasel MSI runs `regsvr32 weaselx64.dll` post-extract to register
// the TSF Text Service. On cold machines (never had Weasel/Rime) the
// HKLM\SOFTWARE\Microsoft\CTF\TIP\{A3F4CDED-…} CLSID subtree doesn't exist,
// and Win+Space won't show TypeAnything until we register it.
//
// DllRegisterServer is idempotent — calling on already-registered system
// just rewrites the same HKCR / HKLM entries.

static bool ColdRegisterTsfDll(const fs::path& dll_path) {
  HMODULE h = LoadLibraryExW(dll_path.c_str(), nullptr,
                              LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) return false;
  using RegFn = HRESULT (WINAPI*)();
  auto fn = (RegFn)GetProcAddress(h, "DllRegisterServer");
  bool ok = false;
  if (fn) ok = SUCCEEDED(fn());
  FreeLibrary(h);
  return ok;
}

static void ColdUnregisterTsfDll(const fs::path& dll_path) {
  HMODULE h = LoadLibraryExW(dll_path.c_str(), nullptr,
                              LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) return;
  using UnregFn = HRESULT (WINAPI*)();
  auto fn = (UnregFn)GetProcAddress(h, "DllUnregisterServer");
  if (fn) fn();
  FreeLibrary(h);
}

// ─── Start Menu shortcut ────────────────────────────────────────
//
// `Win+Space → TypeAnything` works once TSF is registered, but if the
// WeaselServer backend isn't running (no tray icon → no menu), the user
// has no entry point. A Start Menu .lnk → WeaselServer.exe gives them a
// one-click "start the IME backend" with zero UI side-effects.

static fs::path StartMenuProgramsAllUsers() {
  PWSTR p = nullptr; std::wstring root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, NULL, &p))) {
    root = p; CoTaskMemFree(p);
  }
  return fs::path(root);
}

static bool CreateLnk(const fs::path& target_exe,
                      const fs::path& lnk_path,
                      const std::wstring& description) {
  IShellLinkW* psl = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&psl));
  if (FAILED(hr) || !psl) return false;
  psl->SetPath(target_exe.c_str());
  psl->SetDescription(description.c_str());
  psl->SetWorkingDirectory(target_exe.parent_path().c_str());
  psl->SetIconLocation(target_exe.c_str(), 0);

  IPersistFile* ppf = nullptr;
  bool ok = false;
  if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))) && ppf) {
    std::error_code ec;
    fs::create_directories(lnk_path.parent_path(), ec);
    ok = SUCCEEDED(ppf->Save(lnk_path.c_str(), TRUE));
    ppf->Release();
  }
  psl->Release();
  return ok;
}

// ─── TSF profile description patch ──────────────────────────────

static void PatchTsfProfileDescriptions() {
  const wchar_t* clsid = L"{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}";
  const wchar_t* profile_guid = L"{3D02CAB6-2B8E-4781-BA20-1C9267529467}";
  std::wstring root_path = std::wstring(L"SOFTWARE\\Microsoft\\CTF\\TIP\\") + clsid + L"\\LanguageProfile";
  HKEY hRoot;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_path.c_str(), 0,
                    KEY_READ | KEY_WOW64_64KEY, &hRoot) != ERROR_SUCCESS) {
    return;
  }
  // Iterate LCID subkeys.
  WCHAR lcid[64];
  DWORD lcid_len; DWORD idx = 0;
  while (true) {
    lcid_len = sizeof(lcid) / sizeof(wchar_t);
    if (RegEnumKeyExW(hRoot, idx++, lcid, &lcid_len, nullptr,
                      nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    std::wstring prof = root_path + L"\\" + lcid + L"\\" + profile_guid;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, prof.c_str(), 0,
                      KEY_WRITE | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
      const wchar_t* desc = L"TypeAnything";
      RegSetValueExW(h, L"Description", 0, REG_SZ,
                     (const BYTE*)desc,
                     (DWORD)((wcslen(desc) + 1) * sizeof(wchar_t)));
      RegCloseKey(h);
    }
  }
  RegCloseKey(hRoot);
}

// ─── progress thread state ──────────────────────────────────────

struct Progress {
  std::mutex mu;
  std::vector<std::string> queue;   // JSON strings to send to JS
};

static Progress g_progress;

static void Push(const std::string& json_payload) {
  std::lock_guard<std::mutex> lock(g_progress.mu);
  g_progress.queue.push_back(json_payload);
}

static void PushStatus(int percent, const std::string& msg,
                       const std::string& log = "",
                       const std::string& logStatus = "",
                       bool done = false,
                       const std::string& error = "") {
  std::ostringstream j;
  j << "{";
  j << "\"percent\":" << percent;
  if (!msg.empty()) j << ",\"msg\":\"" << JsonEscape(msg) << "\"";
  if (!log.empty()) j << ",\"log\":\"" << JsonEscape(log) << "\"";
  if (!logStatus.empty()) j << ",\"logStatus\":\"" << JsonEscape(logStatus) << "\"";
  if (done) j << ",\"done\":true";
  if (!error.empty()) j << ",\"error\":\"" << JsonEscape(error) << "\"";
  j << "}";
  Push(j.str());
}

// ─── install procedure ─────────────────────────────────────────

struct InstallOptions {
  std::string api_key;
  std::string target_lang;
  std::wstring install_dir;  // empty -> default
};

static void DoInstall(InstallOptions opts) {
  bool reboot_needed = false;

  // 1. Stop running Weasel-family processes.
  PushStatus(5, "停止运行中的输入法服务 …", "停止后台进程");
  for (const wchar_t* img : {L"WeaselServer.exe", L"WeaselDeployer.exe",
                              L"WeaselTrayIcon.exe", L"ta-settings.exe"}) {
    KillByName(img);
  }
  Sleep(800);

  // 2. Resolve install directory + lock it into the registry so every
  //    downstream consumer (the TSF, WeaselServer, ta-settings, the
  //    uninstaller) finds the same path. Falls back to the previous
  //    install or the default if the caller passed nothing.
  fs::path wdir = !opts.install_dir.empty()
                      ? fs::path(opts.install_dir)
                      : WeaselDir();
  WriteWeaselRootRegistry(wdir);
  if (!fs::exists(wdir)) {
    std::error_code ec; fs::create_directories(wdir, ec);
  }

  struct Pair { LPCWSTR res; const wchar_t* leaf; int weight; };
  std::vector<Pair> binaries = {
    {MAKEINTRESOURCEW(IDR_RIME_DLL),       L"rime.dll",          12},
    {MAKEINTRESOURCEW(IDR_WEASELX64_DLL),  L"weaselx64.dll",     12},
    {MAKEINTRESOURCEW(IDR_WEASELSERVER),   L"WeaselServer.exe",  10},
    {MAKEINTRESOURCEW(IDR_WEASELDEPLOYER), L"WeaselDeployer.exe", 8},
    {MAKEINTRESOURCEW(IDR_TA_SETTINGS),    L"ta-settings.exe",    8},
    {MAKEINTRESOURCEW(IDR_WV2_LOADER),     L"WebView2Loader.dll", 6},
  };

  int percent = 10;
  for (auto& b : binaries) {
    fs::path dst = wdir / b.leaf;
    fs::path bak = dst.wstring() + L".bak";
    if (fs::exists(dst) && !fs::exists(bak)) {
      std::error_code ec; fs::copy_file(dst, bak, ec);
    }
    std::string msg = "写入 " + WideToUtf8(b.leaf);
    PushStatus(percent, msg, msg);
    if (!WriteEmbeddedToLockableFile(b.res, dst, &reboot_needed)) {
      PushStatus(percent, "失败：" + WideToUtf8(b.leaf), "失败：" + WideToUtf8(b.leaf), "error",
                 false, "无法写入 " + WideToUtf8(b.leaf));
      return;
    }
    percent += b.weight;
  }

  // 3. Replace system32\weasel.dll (TSF reads its IconFile from this copy).
  //    Soft fail: install can still work without this; sys32 dll is only
  //    used for the IME's tray icon resource path.
  fs::path sys32_dll = L"C:\\WINDOWS\\system32\\weasel.dll";
  fs::path sys32_bak = sys32_dll.wstring() + L".bak";
  if (fs::exists(sys32_dll) && !fs::exists(sys32_bak)) {
    std::error_code ec; fs::copy_file(sys32_dll, sys32_bak, ec);
  }
  PushStatus(64, "替换系统组件 …", "替换系统 DLL");
  if (!WriteEmbeddedToLockableFile(MAKEINTRESOURCEW(IDR_WEASELX64_DLL),
                                   sys32_dll, &reboot_needed)) {
    PushStatus(64, "system32\\weasel.dll 替换失败（不影响主功能）",
               "system32 DLL 写失败（warning）", "warning");
  }

  // 4. ta-settings ui directory.
  fs::path ui = wdir / L"ui";
  PushStatus(70, "释放设置面板资源 …", "ui/ 写入完成");
  WriteEmbeddedToFile(MAKEINTRESOURCEW(IDR_TARGET_UI_HTML), ui / L"index.html");
  WriteEmbeddedToFile(MAKEINTRESOURCEW(IDR_TARGET_UI_CSS),  ui / L"style.css");
  WriteEmbeddedToFile(MAKEINTRESOURCEW(IDR_TARGET_UI_JS),   ui / L"app.js");
  WriteEmbeddedToFile(MAKEINTRESOURCEW(IDR_TARGET_UI_PNG),  ui / L"fish.png");

  // 5. Schema yaml with injected api_key and target_lang.
  PushStatus(78, "写入用户配置 …", "用户配置就绪");
  {
    auto [ptr, sz] = LoadEmbedded(MAKEINTRESOURCEW(IDR_SCHEMA_YAML));
    if (!ptr || sz == 0) {
      PushStatus(78, "失败：内嵌 schema 资源缺失", "IDR_SCHEMA_YAML missing",
                 "error", false, "embedded schema resource not found");
      return;
    }
    std::string yaml((const char*)ptr, sz);
    auto replace_first = [&](const std::string& needle, const std::string& with) {
      size_t pos = yaml.find(needle);
      if (pos != std::string::npos) yaml.replace(pos, needle.size(), with);
    };
    replace_first("api_key: \"\"", "api_key: \"" + opts.api_key + "\"");
    replace_first("target_lang: English",
                  "target_lang: " + (opts.target_lang.empty() ? "English" : opts.target_lang));
    fs::path udir = RimeUserDir();
    std::error_code ec; fs::create_directories(udir, ec);
    std::ofstream f(udir / L"typeanything.schema.yaml", std::ios::binary | std::ios::trunc);
    f.write(yaml.data(), (std::streamsize)yaml.size());

    // Supplement dict (modern AI / IT / social-media / slang terms that
    // luna_pinyin lacks). Schema's translator imports luna_pinyin via
    // typeanything.dict.yaml, so this file is required for the schema to
    // compile.
    if (auto [dptr, dsz] = LoadEmbedded(MAKEINTRESOURCEW(IDR_DICT_YAML));
        dptr && dsz > 0) {
      std::ofstream df(udir / L"typeanything.dict.yaml",
                       std::ios::binary | std::ios::trunc);
      df.write((const char*)dptr, (std::streamsize)dsz);
    }

    // Default custom.yaml — pin the schema list to TypeAnything, set
    // menu page_size to 7 (Microsoft-IME parity), and force non-inline
    // preedit in Office apps where inline-mode TSF caret reporting is
    // broken (candidate bar would otherwise pin to the window's top-left).
    std::ofstream cf(udir / L"default.custom.yaml", std::ios::binary | std::ios::trunc);
    cf << "patch:\n"
       << "  schema_list:\n"
       << "    - schema: typeanything\n"
       << "  \"menu/page_size\": 7\n"
       << "  app_options/winword.exe:\n    inline_preedit: false\n"
       << "  app_options/wps.exe:\n    inline_preedit: false\n"
       << "  app_options/wpp.exe:\n    inline_preedit: false\n"
       << "  app_options/excel.exe:\n    inline_preedit: false\n"
       << "  app_options/powerpnt.exe:\n    inline_preedit: false\n";

    // Weasel UI: Microsoft IME look — horizontal pill, inline preedit
    // (pinyin stays at the editor cursor, not duplicated in the candidate
    // bar), YaHei UI font, tighter spacing, light-gray hilite (no big
    // blue selection box). Always overwrite the patch block so a
    // reinstall reliably restores the intended style.
    fs::path wcust = udir / L"weasel.custom.yaml";
    {
      std::ofstream wf(wcust, std::ios::binary | std::ios::trunc);
      wf <<
        "# Managed by TypeAnything installer — Microsoft-IME-style layout.\n"
        "patch:\n"
        "  \"style/horizontal\": true\n"
        "  \"style/inline_preedit\": true\n"
        "  \"style/label_format\": \"%s \"\n"
        "  \"style/font_face\": \"Microsoft YaHei UI\"\n"
        "  \"style/font_point\": 11\n"
        "  \"style/label_font_point\": 11\n"
        "  \"style/comment_font_point\": 11\n"
        "  \"style/margin_x\": 8\n"
        "  \"style/margin_y\": 2\n"
        "  \"style/hilite_padding\": 6\n"
        "  \"style/hilite_spacing\": 0\n"
        "  \"style/candidate_spacing\": 22\n"
        "  \"style/color_scheme\": typeanything_light\n"
        "  \"preset_color_schemes/typeanything_light\":\n"
        "    name: TypeAnything Light\n"
        "    author: HRDAI\n"
        "    back_color: 0xFFFFFF\n"
        "    border_color: 0xCCCCCC\n"
        "    text_color: 0x000000\n"
        "    hilited_text_color: 0x000000\n"
        "    hilited_back_color: 0xFFFFFF\n"
        "    candidate_text_color: 0x202020\n"
        // Transparent so cells inherit panel back_color — kills the
        // per-cell white stripe artifact between candidates.
        "    candidate_back_color: 0x00000000\n"
        "    hilited_candidate_text_color: 0x000000\n"
        "    hilited_candidate_back_color: 0xE6E6E6\n"
        "    comment_text_color: 0x808080\n"
        "    hilited_comment_text_color: 0x808080\n"
        "    label_color: 0x999999\n"
        "    hilited_candidate_label_color: 0xD47800\n"
        // Alpha byte must be 0xFF for Weasel's page_en /\n"
        // hilited_mark color-not-transparent checks to pass.\n"
        "    hilited_mark_color: 0xFFD47800\n"
        "    prevpage_color: 0xFF303030\n"
        "    nextpage_color: 0xFF303030\n";
    }

    // Seed lang.txt with target (so ResolveTargetLang has something on Enter
    // before user opens the settings panel).
    std::ofstream lf(udir / L"typeanything_lang.txt", std::ios::binary | std::ios::trunc);
    lf << (opts.target_lang.empty() ? "English" : opts.target_lang);
  }

  // 6. Deploy Rime base data (luna_pinyin + presets) to <wdir>\data\.
  //    Required for cold machines (never had Weasel/Rime). Skip files
  //    that already exist — preserves user-trained luna_pinyin user_dict
  //    on upgrade installs.
  PushStatus(82, "释放拼音字典与预设 …", "Rime 数据文件");
  {
    fs::path data = wdir / L"data";
    std::error_code ec; fs::create_directories(data, ec);
    struct DataFile { LPCWSTR res; const wchar_t* name; };
    std::vector<DataFile> data_files = {
      {MAKEINTRESOURCEW(IDR_DATA_DEFAULT),       L"default.yaml"},
      {MAKEINTRESOURCEW(IDR_DATA_LUNA_DICT),     L"luna_pinyin.dict.yaml"},
      {MAKEINTRESOURCEW(IDR_DATA_LUNA_SCHEMA),   L"luna_pinyin.schema.yaml"},
      {MAKEINTRESOURCEW(IDR_DATA_ESSAY),         L"essay.txt"},
      {MAKEINTRESOURCEW(IDR_DATA_SYMBOLS),       L"symbols.yaml"},
      {MAKEINTRESOURCEW(IDR_DATA_PUNCTUATION),   L"punctuation.yaml"},
      {MAKEINTRESOURCEW(IDR_DATA_KEY_BINDINGS),  L"key_bindings.yaml"},
    };
    int missing = 0;
    for (auto& d : data_files) {
      fs::path dst = data / d.name;
      if (fs::exists(dst)) continue;   // preserve existing
      if (!WriteEmbeddedToFile(d.res, dst)) {
        ++missing;
        PushStatus(82, std::string("数据文件写失败：") + WideToUtf8(d.name),
                   std::string("写失败：") + WideToUtf8(d.name), "warning");
      }
    }
    if (missing > 0 && !fs::exists(data / L"luna_pinyin.dict.yaml")) {
      // luna_pinyin missing = schema cannot compile.
      PushStatus(82, "拼音字典未就绪，无法继续",
                 "luna_pinyin.dict.yaml 缺失", "error", false,
                 "Rime 字典文件无法部署（杀软拦截？磁盘只读？）");
      return;
    }
  }

  // 7. Cold-register weaselx64.dll as a TSF text service.
  //    Standard Weasel MSI does this via regsvr32 — ta-installer must do
  //    it itself for cold machines, otherwise Win+Space won't show
  //    TypeAnything. Idempotent on already-registered systems.
  PushStatus(84, "注册到 Windows 输入法框架 …", "TSF 注册 (DllRegisterServer)");
  if (!ColdRegisterTsfDll(wdir / L"weaselx64.dll")) {
    PushStatus(84, "TSF 注册失败 — 检查 weaselx64.dll 是否被杀软拦截",
               "DllRegisterServer 失败", "error", false,
               "无法把 TypeAnything 注册到 Windows 输入法框架。"
               "通常是杀软 / EDR / GPO 拦截，或 weaselx64.dll 文件被改坏。");
    return;
  }

  // 8. Patch HKLM TSF profile descriptions (now that the TIP CLSID
  //    subtree exists from step 7 above).
  PushStatus(86, "改写输入法显示名称 …", "TSF 描述改写为 TypeAnything");
  PatchTsfProfileDescriptions();

  // 9. Drop other schemas from Weasel data dir (keep luna_pinyin + typeanything).
  PushStatus(88, "仅保留 TypeAnything 方案 …", "隐藏其他 Rime 方案");
  {
    fs::path data = wdir / L"data";
    fs::path orig = wdir / L"data.original";
    if (fs::exists(data) && !fs::exists(orig)) {
      std::error_code ec; fs::copy(data, orig, fs::copy_options::recursive, ec);
    }
    if (fs::exists(data)) {
      for (auto& e : fs::directory_iterator(data)) {
        auto name = e.path().filename().wstring();
        if (name.size() > 12 && name.substr(name.size() - 12) == L".schema.yaml"
            && name.compare(0, 11, L"luna_pinyin") != 0
            && name.compare(0, 12, L"typeanything") != 0) {
          std::error_code ec; fs::remove(e.path(), ec);
        }
        if (name.size() > 10 && name.substr(name.size() - 10) == L".dict.yaml"
            && name.compare(0, 11, L"luna_pinyin") != 0
            && name.compare(0, 12, L"typeanything") != 0) {
          std::error_code ec; fs::remove(e.path(), ec);
        }
      }
    }
  }

  // 8. Redeploy schema + start server.
  PushStatus(92, "编译输入方案 …", "Rime 编译中（首次约 10-30 秒）");
  StartHidden(wdir / L"WeaselDeployer.exe", L"/deploy");

  // Poll for typeanything.prism.bin freshness, max 60s.
  fs::path prism = RimeUserDir() / L"build" / L"typeanything.prism.bin";
  auto start = std::chrono::steady_clock::now();
  auto last_mtime_ok = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
    if (fs::exists(prism)) {
      auto ft = fs::last_write_time(prism);
      auto now = decltype(ft)::clock::now();
      auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ft).count();
      if (age < 30) { last_mtime_ok = true; break; }
    }
    Sleep(500);
  }
  KillByName(L"WeaselDeployer.exe");

  if (!last_mtime_ok) {
    PushStatus(95, "编译耗时偏长，继续启动服务", "编译超时回退，跳过等待",
               "done");
  } else {
    PushStatus(95, "方案编译完成", "方案编译完成", "done");
  }

  // 10. Start WeaselServer so the tray icon comes up. Fatal if it
  //     doesn't start — without server there's no tray menu, no IPC
  //     backend for the TSF DLL.
  PushStatus(96, "启动输入法服务 …", "输入法服务上线");
  if (!StartShown(wdir / L"WeaselServer.exe")) {
    PushStatus(96, "WeaselServer.exe 启动失败",
               "无法启动 WeaselServer.exe", "error", false,
               "WeaselServer.exe 启动失败（杀软拦截 / 缺少 VC++ 运行时 / "
               "rime.dll 加载失败）。");
    return;
  }

  // 11. Register autostart on next login.
  {
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
      std::wstring exe = (wdir / L"WeaselServer.exe").wstring();
      RegSetValueExW(h, L"WeaselServer", 0, REG_SZ,
                     (const BYTE*)exe.c_str(),
                     (DWORD)((exe.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(h);
    }
  }

  // 12. Drop a copy of ourselves as `uninstall.exe` + Add/Remove Programs entry.
  {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, _countof(self));
    fs::path uninst = wdir / L"uninstall.exe";
    std::error_code ec;
    fs::copy_file(fs::path(self), uninst,
                  fs::copy_options::overwrite_existing, ec);
    WriteUninstallRegistry(wdir, L"0.6.3");
  }

  // 13. Start Menu shortcut → WeaselServer.exe. User-facing "start the
  //     input method" entry. Lives in the all-users Start Menu so it
  //     shows up for every account on the machine.
  PushStatus(98, "创建开始菜单快捷方式 …", "开始菜单 → TypeAnything");
  {
    fs::path lnk = StartMenuProgramsAllUsers() / L"TypeAnything.lnk";
    if (!CreateLnk(wdir / L"WeaselServer.exe", lnk,
                   L"启动 TypeAnything 输入法")) {
      PushStatus(98, "开始菜单快捷方式创建失败（不影响主功能）",
                 "Start Menu lnk 失败（warning）", "warning");
    }
  }

  std::string final_msg = reboot_needed
      ? "部分文件被占用，已排队重启替换。重启电脑后完全生效。"
      : "全部完成。打开新应用即可使用。";
  PushStatus(100, final_msg, final_msg, "done", true);
}

// ─── uninstall procedure ────────────────────────────────────────
//
// Reverses what DoInstall did:
//   * Kill running Weasel-family processes.
//   * Remove HKLM\Run autostart, HKLM CTF TIP profile description patches,
//     WeaselRoot, Uninstall reg key.
//   * Replace system32\weasel.dll with its .bak (if any), otherwise queue
//     deletion on reboot.
//   * Delete the install directory.
//   * %APPDATA%\Rime is preserved (user dict + custom yamls stay so a
//     later reinstall doesn't lose typing history). Caller may delete it.

static void DeleteRegKey64(HKEY root, const wchar_t* sub) {
  RegDeleteKeyExW(root, sub, KEY_WOW64_64KEY, 0);
}
static void DeleteRegValue64(HKEY root, const wchar_t* sub,
                             const wchar_t* val) {
  HKEY h;
  if (RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h)
      == ERROR_SUCCESS) {
    RegDeleteValueW(h, val);
    RegCloseKey(h);
  }
}

static void DoUninstall() {
  PushStatus(2, "停止运行中的输入法服务 …", "停止后台进程");
  for (const wchar_t* img : {L"WeaselServer.exe", L"WeaselDeployer.exe",
                              L"WeaselTrayIcon.exe", L"ta-settings.exe"}) {
    KillByName(img);
  }
  Sleep(800);

  fs::path wdir = WeaselDir();

  // Unregister TSF (DllUnregisterServer) while weaselx64.dll is still on
  // disk. Removes the HKLM CTF\TIP CLSID subtree the installer created.
  PushStatus(8, "从输入法框架注销 …", "TSF 注销 (DllUnregisterServer)");
  ColdUnregisterTsfDll(wdir / L"weaselx64.dll");

  // Remove Start Menu shortcut.
  {
    fs::path lnk = StartMenuProgramsAllUsers() / L"TypeAnything.lnk";
    std::error_code ec; fs::remove(lnk, ec);
  }

  PushStatus(15, "撤销注册表项 …", "Run / Uninstall / WeaselRoot");
  DeleteRegValue64(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                   L"WeaselServer");
  DeleteRegKey64(HKEY_LOCAL_MACHINE,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TypeAnything");
  // WeaselRoot — keep until LAST so that anything reading it during
  // uninstall still resolves correctly.

  // Best-effort restore of system32\weasel.dll from its .bak (saved by
  // an earlier install). If the file is in use it can be renamed but
  // not overwritten, same trick the installer uses.
  PushStatus(40, "还原系统组件 …", "system32\\weasel.dll");
  {
    fs::path sys32 = L"C:\\WINDOWS\\system32\\weasel.dll";
    fs::path bak = sys32.wstring() + L".bak";
    std::error_code ec;
    if (fs::exists(bak)) {
      // Rename current (locked) out, copy bak in.
      auto stamp = std::to_string((long long)time(NULL));
      fs::path moved = sys32.wstring() + L".uninstalled-" +
                       std::wstring(stamp.begin(), stamp.end());
      fs::rename(sys32, moved, ec);
      fs::copy_file(bak, sys32, fs::copy_options::overwrite_existing, ec);
    } else {
      // No backup — queue delete on reboot. Don't try to delete now
      // (TSF probably has it mapped).
      MoveFileExW(sys32.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
  }

  // Delete the install directory. Don't fail the whole uninstall on
  // individual stuck files — log them and continue.
  PushStatus(70, "删除安装目录 …", "files in install dir");
  if (fs::exists(wdir)) {
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(
             wdir, fs::directory_options::skip_permission_denied, ec)) {
      if (e.is_regular_file(ec)) {
        std::error_code ec2;
        if (!fs::remove(e.path(), ec2)) {
          MoveFileExW(e.path().c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
      }
    }
    fs::remove_all(wdir, ec);  // sweep remaining (incl. ourselves)
  }

  PushStatus(95, "清理注册表 …", "WeaselRoot");
  DeleteRegKey64(HKEY_LOCAL_MACHINE, L"Software\\Rime\\Weasel");

  // Self-cleanup. We're running from <wdir>\uninstall.exe — fs::remove
  // can't delete a running .exe and MoveFileEx DELAY_UNTIL_REBOOT only
  // wipes it after a reboot. Spawn a detached cmd.exe that waits a few
  // seconds (for the user to close the done dialog and us to exit) then
  // recursively removes the install dir. cmd.exe is in system32 not
  // <wdir>, so it isn't affected by the dir going away.
  {
    std::wstring cmd_line =
        L"cmd.exe /c ping -n 4 127.0.0.1 > nul & rmdir /s /q \""
        + wdir.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd_line.data(),
                       nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS,
                       nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }

  // We intentionally do NOT delete %APPDATA%\Rime — that contains the
  // user's learned dictionary + custom config. A later reinstall picks
  // up where they left off.
  PushStatus(100, "卸载完成。", "%APPDATA%\\Rime 保留（用户配置）", "done", true);
}

// ─── main ──────────────────────────────────────────────────────

static void ApplyMica(HWND hwnd) {
  enum DWMSBT { DWMSBT_MAINWINDOW = 2 };
  const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
  const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
  int backdrop = DWMSBT_MAINWINDOW;
  DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static std::string FileUrl(const fs::path& p) {
  std::wstring w = p.wstring();
  for (auto& c : w) if (c == L'\\') c = L'/';
  return "file:///" + WideToUtf8(w);
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
  // CoInitialize for IFileOpenDialog (install-dir picker).
  ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  // Mode: install (default) or uninstall (when invoked with /uninstall).
  const bool uninstall_mode = lpCmdLine && wcsstr(lpCmdLine, L"/uninstall");
  if (uninstall_mode) {
    // Up-front confirmation. If the user backs out here we don't even
    // bother spinning up WebView2.
    int yn = MessageBoxW(
        NULL,
        L"确定要卸载 TypeAnything 输入法吗？\n\n"
        L"会清除安装目录与系统注册项。\n"
        L"用户字典与个人配置（%APPDATA%\\Rime）会保留。",
        L"卸载 TypeAnything",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (yn != IDYES) {
      ::CoUninitialize();
      return 0;
    }
  }

  // Force a writable WebView2 user data folder (Program Files is read-only).
  wchar_t lad[MAX_PATH] = {0};
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, lad))) {
    std::wstring ud = std::wstring(lad) + L"\\TypeAnything\\WebView2-installer";
    std::error_code ec; fs::create_directories(ud, ec);
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", ud.c_str());
  }

  // Materialize inline UI to %LOCALAPPDATA%\TypeAnything\installer-ui\index.html
  // and navigate the webview to it via file://. Avoids the data: URL nesting
  // and length limits we hit when calling set_html() with embedded base64.
  std::wstring local_appdata;
  {
    wchar_t buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf))) {
      local_appdata = buf;
    }
  }
  fs::path ui_dir = fs::path(local_appdata) / L"TypeAnything" / L"installer-ui";
  std::error_code ui_ec;
  fs::create_directories(ui_dir, ui_ec);
  fs::path ui_html = ui_dir / L"index.html";
  {
    std::string inline_html = BuildInlineHtml();
    // Mode flag for the UI JS — switches welcome/progress/done copy.
    std::string mode_script =
        std::string("<script>window.TA_MODE=\"") +
        (uninstall_mode ? "uninstall" : "install") + "\";</script>\n";
    // Inject right after <head> so JS sees it before app.js runs.
    auto pos = inline_html.find("<head>");
    if (pos != std::string::npos) {
      inline_html.insert(pos + 6, "\n" + mode_script);
    }
    std::ofstream f(ui_html, std::ios::binary | std::ios::trunc);
    f.write(inline_html.data(), (std::streamsize)inline_html.size());
  }

  webview::webview w(false, nullptr);
  w.set_title(uninstall_mode ? "TypeAnything 卸载" : "TypeAnything 安装");
  // Fixed size — installer is a dialog, not a resizeable workspace.
  // Also dodges WebView2 user-data window-state persistence quirks.
  w.set_size(640, 420, WEBVIEW_HINT_FIXED);

  std::atomic<bool> installing{false};
  // Default install directory shown in the picker. If a previous install
  // wrote WeaselRoot we honor it; otherwise Program Files default.
  std::wstring default_install_dir = WeaselDir().wstring();

  // Bridge: pick an install directory via the system folder dialog. JS
  // calls this and receives the chosen path as a JSON string (empty on
  // cancel).
  w.bind("nativePickInstallDir", [&](const std::string& /*args*/) -> std::string {
    HWND parent = (HWND)w.window();
    std::string picked = PickInstallDir(parent, default_install_dir);
    // Return as a JSON string literal so JS can JSON.parse the result.
    std::string esc = JsonEscape(picked);
    return "\"" + esc + "\"";
  });

  // Bridge: begin install. UI passes the chosen install directory (UTF-8
  // string). API key + default target are NOT collected here — schema
  // yaml is written with empty api_key + "English" target, and the user
  // is guided to the model-config panel on the done screen.
  w.bind("nativeBeginInstall", [&](const std::string& args) -> std::string {
    if (installing.exchange(true)) return "false";
    auto p = ParseJsonStringArray(args);
    std::wstring dir = p.empty() ? std::wstring() : Utf8ToWide(p[0]);
    InstallOptions opts{"", "English", dir};
    std::thread([opts]() { DoInstall(opts); }).detach();
    return "true";
  });

  // Bridge: begin uninstall.
  w.bind("nativeBeginUninstall", [&](const std::string& /*args*/) -> std::string {
    if (installing.exchange(true)) return "false";
    std::thread([]() { DoUninstall(); }).detach();
    return "true";
  });

  // Bridge: report the default install dir so the UI can prefill.
  w.bind("nativeDefaultInstallDir", [&](const std::string&) -> std::string {
    return "\"" + JsonEscape(WideToUtf8(default_install_dir)) + "\"";
  });

  // Open the ta-settings.exe panel after install. Arg = "model" | "lang".
  w.bind("nativeOpenSettings", [](const std::string& args) -> std::string {
    auto p = ParseJsonStringArray(args);
    std::string page = p.empty() ? std::string("model") : p[0];
    fs::path exe = WeaselDir() / L"ta-settings.exe";
    std::wstring wargs = std::wstring(L"--page ") + Utf8ToWide(page);
    ShellExecuteW(nullptr, L"open", exe.c_str(), wargs.c_str(),
                  nullptr, SW_SHOWNORMAL);
    return "true";
  });

  w.bind("nativeOpenUrl", [](const std::string& args) -> std::string {
    auto p = ParseJsonStringArray(args);
    if (p.empty()) return "false";
    ShellExecuteW(nullptr, L"open", Utf8ToWide(p[0]).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
    return "true";
  });

  w.bind("nativeClose", [&](const std::string&) -> std::string {
    w.terminate();
    return "true";
  });

  // Window + Mica.
  HWND hwnd = (HWND)w.window();
  ApplyMica(hwnd);

  // Progress pump: every 100ms drain g_progress.queue and forward to JS.
  std::atomic<bool> stop_pump{false};
  std::thread pump([&]() {
    while (!stop_pump.load()) {
      std::vector<std::string> batch;
      {
        std::lock_guard<std::mutex> lock(g_progress.mu);
        batch.swap(g_progress.queue);
      }
      for (auto& json : batch) {
        std::string js = "window.installerStatus(" + json + ");";
        try { w.dispatch([&w, js]() { w.eval(js); }); }
        catch (...) {}
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
  });

  w.navigate(FileUrl(ui_html));
  w.run();

  stop_pump.store(true);
  pump.join();
  return 0;
}
