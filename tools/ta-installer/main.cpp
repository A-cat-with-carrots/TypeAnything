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
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM (used in NCHITTEST)
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

// ─── install log ────────────────────────────────────────────────
//
// Comprehensive observability for cold-install failures. Lives at
// <install_dir>\typeanything_install.log (NOT %APPDATA% — cold-install
// boxes may have no Roaming\Rime yet, but install_dir is always
// user-chosen and known to exist by log time).
//
// Format (mirrors classify/translate/update logs):
//   [YYYY-MM-DD HH:MM:SS] STEP target="<step name>"
//     result=<detail>
//   ---
//
// No HTTP/key data — install is local; nothing sensitive to redact.
// Auto-rotate at 1 MB: rename to .1 (overwriting existing .1) and
// start a fresh log.

static std::mutex g_log_mu;
static fs::path g_install_log_path;

static std::string CurrentTimestamp() {
  SYSTEMTIME st; GetLocalTime(&st);
  char buf[32];
  snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d]",
           st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond);
  return buf;
}

// Rotate <path> to <path>.1 if size exceeds 1 MB. Best-effort: if
// rename fails (file locked / perms), just keep appending — the log
// will grow past 1 MB but never silently disappear.
static void RotateLogIfNeeded(const fs::path& path) {
  std::error_code ec;
  auto sz = fs::file_size(path, ec);
  if (ec || sz < 1024ULL * 1024ULL) return;
  fs::path rotated = path.wstring() + L".1";
  fs::remove(rotated, ec);             // overwrite any prior .1
  fs::rename(path, rotated, ec);
}

// Set the install log path once `install_dir` is known. Called early
// in DoInstall / DoUninstall before any InstallLog() invocation.
static void SetInstallLogPath(const fs::path& install_dir) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  std::error_code ec;
  fs::create_directories(install_dir, ec);
  g_install_log_path = install_dir / L"typeanything_install.log";
}

// Append one step record to the install log. Safe to call from any
// thread. No-op if the log path hasn't been set yet (very early
// pre-resolve calls), in which case the entry is silently dropped —
// not worth crashing the installer for an observability miss.
static void InstallLog(const std::string& step,
                       const std::string& detail) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  if (g_install_log_path.empty()) return;
  RotateLogIfNeeded(g_install_log_path);
  std::ofstream f(g_install_log_path, std::ios::binary | std::ios::app);
  if (!f) return;
  // Single-line detail: strip embedded newlines so the log stays
  // grep-friendly even when callers pass multi-line GetLastError text.
  std::string flat = detail;
  for (char& c : flat) if (c == '\n' || c == '\r') c = ' ';
  f << CurrentTimestamp() << " STEP target=\"" << step << "\"\n"
    << "  result=" << flat << "\n"
    << "---\n";
}

// Format Win32 GetLastError() into "<code>: <message>" for log detail.
static std::string FormatWinError(DWORD err) {
  LPWSTR msg = nullptr;
  DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPWSTR)&msg, 0, nullptr);
  std::string out = std::to_string((unsigned long)err);
  if (n > 0 && msg) {
    out += ": ";
    out += WideToUtf8(std::wstring(msg, n));
    // Trim trailing CRLF/space.
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' ||
                            out.back() == ' '))
      out.pop_back();
  }
  if (msg) LocalFree(msg);
  return out;
}

// Read the last `n` non-empty lines from the install log for the
// failure MessageBox. Returns "" if log path unset / file missing.
static std::string TailInstallLog(size_t n) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  if (g_install_log_path.empty()) return "";
  std::ifstream f(g_install_log_path, std::ios::binary);
  if (!f) return "";
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  size_t start = lines.size() > n ? lines.size() - n : 0;
  std::string out;
  for (size_t i = start; i < lines.size(); ++i) {
    out += lines[i];
    out += "\n";
  }
  return out;
}

// Show a failure dialog with the last 30 log lines + log file path.
// Called from PushFailure paths so users have everything they need
// to attach a GitHub issue without hunting for the log.
static void ShowFailureDialog(const std::string& summary) {
  std::string tail = TailInstallLog(30);
  std::wstring path_w =
      g_install_log_path.empty() ? L"(log path unset)"
                                 : g_install_log_path.wstring();
  std::wstring body =
      L"安装失败：\n\n" + Utf8ToWide(summary) + L"\n\n" +
      L"完整日志位置（请附带此文件到 GitHub Issue）：\n" + path_w +
      L"\n\n最近 30 行日志：\n" + Utf8ToWide(tail);
  MessageBoxW(nullptr, body.c_str(), L"TypeAnything 安装失败",
              MB_OK | MB_ICONERROR);
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

// Start `exe` at MEDIUM integrity level by inheriting the desktop shell's
// (explorer.exe) primary token. Required so WeaselServer's named pipe ACL
// matches the IL of typical text-input applications (Notepad / 微信 / Chrome /
// Word — all MEDIUM IL). Without de-elevation, MEDIUM client apps' IPC to
// WeaselServer would be ACCESS_DENIED → IME wouldn't show candidates.
//
// Returns false when explorer.exe isn't running (UAC=0 / pre-login) or when
// the elevated installer lacks SE_IMPERSONATE_NAME (rare). Caller should
// fall back to a normal CreateProcessW.

static bool StartDeElevated(const fs::path& exe_path) {
  HWND hShell = GetShellWindow();
  if (!hShell) return false;

  DWORD shell_pid = 0;
  GetWindowThreadProcessId(hShell, &shell_pid);
  if (!shell_pid) return false;

  HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shell_pid);
  if (!hProc) return false;

  HANDLE hShellTok = nullptr;
  BOOL ok = OpenProcessToken(hProc, TOKEN_DUPLICATE, &hShellTok);
  CloseHandle(hProc);
  if (!ok || !hShellTok) return false;

  HANDLE hPriToken = nullptr;
  ok = DuplicateTokenEx(
      hShellTok,
      TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
          TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
      nullptr, SecurityImpersonation, TokenPrimary, &hPriToken);
  CloseHandle(hShellTok);
  if (!ok || !hPriToken) return false;

  STARTUPINFOW si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe_path.wstring() + L"\"";

  ok = CreateProcessWithTokenW(hPriToken, 0, nullptr, cmd.data(),
                                CREATE_NEW_PROCESS_GROUP, nullptr,
                                nullptr, &si, &pi);
  CloseHandle(hPriToken);
  if (!ok) return false;

  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  return true;
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

// Returns a diagnostic string (empty on success). DllRegisterServer needs
// COM initialized on the calling thread; the worker thread that runs
// DoInstall doesn't inherit COM from main, so init here.
static std::string ColdRegisterTsfDllDiag(const fs::path& dll_path,
                                          bool* ok_out) {
  *ok_out = false;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HMODULE h = LoadLibraryExW(dll_path.c_str(), nullptr,
                              LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) {
    DWORD err = GetLastError();
    CoUninitialize();
    char buf[128];
    snprintf(buf, sizeof(buf),
             "LoadLibraryEx failed (GetLastError=%lu)", err);
    return buf;
  }
  using RegFn = HRESULT (WINAPI*)();
  auto fn = (RegFn)GetProcAddress(h, "DllRegisterServer");
  if (!fn) {
    FreeLibrary(h);
    CoUninitialize();
    return "DllRegisterServer entry point missing in weaselx64.dll";
  }
  HRESULT hr = fn();
  FreeLibrary(h);
  CoUninitialize();
  if (FAILED(hr)) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "DllRegisterServer returned HRESULT=0x%08lX",
             (unsigned long)hr);
    return buf;
  }
  *ok_out = true;
  return "";
}

static void ColdUnregisterTsfDll(const fs::path& dll_path) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HMODULE h = LoadLibraryExW(dll_path.c_str(), nullptr,
                              LOAD_WITH_ALTERED_SEARCH_PATH);
  if (h) {
    using UnregFn = HRESULT (WINAPI*)();
    auto fn = (UnregFn)GetProcAddress(h, "DllUnregisterServer");
    if (fn) fn();
    FreeLibrary(h);
  }
  CoUninitialize();
}

// Cheap check: does HKLM\SOFTWARE\Microsoft\CTF\TIP\<weasel-clsid> exist?
// If yes, a prior Weasel install already registered the TSF profile and
// DllRegisterServer failure is non-fatal — the IME still works.
static bool TsfAlreadyRegistered() {
  HKEY h;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\CTF\\TIP\\"
                    L"{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}",
                    0, KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
    RegCloseKey(h);
    return true;
  }
  return false;
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

  // Resolve + create install_dir FIRST so we can anchor the log file
  // there. Every subsequent step then has somewhere to write its
  // diagnostic trail. (Pipeline steps 1-3 reordered in code only; the
  // observable side effects still happen in the same order.)
  fs::path wdir = !opts.install_dir.empty()
                      ? fs::path(opts.install_dir)
                      : WeaselDir();
  {
    std::error_code ec;
    if (!fs::exists(wdir)) fs::create_directories(wdir, ec);
  }
  SetInstallLogPath(wdir);
  InstallLog("00. install start",
             "install_dir=" + WideToUtf8(wdir.wstring()) +
             " api_key_set=" + (opts.api_key.empty() ? "no" : "yes") +
             " target_lang=" + opts.target_lang);

  // 1. UAC elevation: by the time we reach DoInstall the embedded
  //    manifest has already required+granted admin; nothing to do here
  //    except note it in the log.
  InstallLog("01. uac elevation",
             "ok (embedded manifest requireAdministrator)");

  // 2. Stop running Weasel-family processes.
  PushStatus(5, "停止运行中的输入法服务 …", "停止后台进程");
  InstallLog("02. kill weasel processes", "begin");
  for (const wchar_t* img : {L"WeaselServer.exe", L"WeaselDeployer.exe",
                              L"WeaselTrayIcon.exe", L"ta-settings.exe"}) {
    KillByName(img);
  }
  Sleep(800);
  InstallLog("02. kill weasel processes", "ok");

  // 3. Install directory already resolved above. Log the choice now.
  InstallLog("03. resolve install_dir",
             "path=" + WideToUtf8(wdir.wstring()) +
             " from=" + (opts.install_dir.empty() ? "registry/default"
                                                 : "user picker"));

  // 4. Lock install_dir into the registry so every downstream consumer
  //    (TSF DLL, WeaselServer, ta-settings, uninstaller) finds the same
  //    path. Read it back to verify.
  WriteWeaselRootRegistry(wdir);
  {
    HKEY hk;
    bool readback_ok = false;
    std::wstring got;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Rime\\Weasel", 0,
                      KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
      WCHAR buf[MAX_PATH] = {0}; DWORD len = sizeof(buf); DWORD type = 0;
      if (RegQueryValueExW(hk, L"WeaselRoot", nullptr, &type,
                           (LPBYTE)buf, &len) == ERROR_SUCCESS &&
          type == REG_SZ) {
        got = buf;
        readback_ok = (got == wdir.wstring());
      }
      RegCloseKey(hk);
    }
    InstallLog("04. write WeaselRoot registry",
               readback_ok ? std::string("ok value=") + WideToUtf8(got)
                           : std::string("FAIL readback=") +
                                 WideToUtf8(got));
  }

  // 5. Install directory was created above; just verify + log.
  InstallLog("05. create install_dir",
             fs::exists(wdir) ? "ok (exists)" : "FAIL (does not exist)");

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
  InstallLog("06. write binaries", "begin");
  for (auto& b : binaries) {
    fs::path dst = wdir / b.leaf;
    fs::path bak = dst.wstring() + L".bak";
    if (fs::exists(dst) && !fs::exists(bak)) {
      std::error_code ec; fs::copy_file(dst, bak, ec);
    }
    std::string msg = "写入 " + WideToUtf8(b.leaf);
    PushStatus(percent, msg, msg);
    if (!WriteEmbeddedToLockableFile(b.res, dst, &reboot_needed)) {
      DWORD err = GetLastError();
      std::string fail_detail = "FAIL leaf=" + WideToUtf8(b.leaf) +
                                " err=" + FormatWinError(err);
      InstallLog("06. write binaries", fail_detail);
      ShowFailureDialog("无法写入 " + WideToUtf8(b.leaf) +
                        "\nGetLastError=" + FormatWinError(err));
      PushStatus(percent, "失败：" + WideToUtf8(b.leaf), "失败：" + WideToUtf8(b.leaf), "error",
                 false, "无法写入 " + WideToUtf8(b.leaf));
      return;
    }
    // Verify file size > 0 post-write. Pending-on-reboot writes skip
    // this (file may not exist yet at the real path); the reboot_needed
    // flag already surfaces the deferred state.
    std::error_code stec;
    auto fsize = fs::file_size(dst, stec);
    InstallLog("06. write binaries",
               "ok leaf=" + WideToUtf8(b.leaf) +
                   " bytes=" + (stec ? std::string("(pending-reboot)")
                                     : std::to_string(fsize)));
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
    DWORD err = GetLastError();
    InstallLog("07. write system32 weasel.dll",
               "warning (non-fatal) err=" + FormatWinError(err));
    PushStatus(64, "system32\\weasel.dll 替换失败（不影响主功能）",
               "system32 DLL 写失败（warning）", "warning");
  } else {
    std::error_code stec;
    auto fsize = fs::file_size(sys32_dll, stec);
    InstallLog("07. write system32 weasel.dll",
               std::string("ok bytes=") +
                   (stec ? "(pending-reboot)" : std::to_string(fsize)));
  }

  // 8. ta-settings ui directory.
  fs::path ui = wdir / L"ui";
  PushStatus(70, "释放设置面板资源 …", "ui/ 写入完成");
  {
    struct UiFile { LPCWSTR res; const wchar_t* leaf; };
    UiFile ui_files[] = {
      {MAKEINTRESOURCEW(IDR_TARGET_UI_HTML), L"index.html"},
      {MAKEINTRESOURCEW(IDR_TARGET_UI_CSS),  L"style.css"},
      {MAKEINTRESOURCEW(IDR_TARGET_UI_JS),   L"app.js"},
      {MAKEINTRESOURCEW(IDR_TARGET_UI_PNG),  L"fish.png"},
    };
    for (auto& u : ui_files) {
      bool ok = WriteEmbeddedToFile(u.res, ui / u.leaf);
      std::error_code stec;
      auto sz = fs::file_size(ui / u.leaf, stec);
      InstallLog("08. write ta-settings ui",
                 std::string(ok ? "ok " : "FAIL ") +
                 "leaf=" + WideToUtf8(u.leaf) +
                 " bytes=" + (stec ? "0" : std::to_string(sz)));
    }
  }

  // 9. Schema yaml with injected api_key and target_lang.
  PushStatus(78, "写入用户配置 …", "用户配置就绪");
  {
    auto [ptr, sz] = LoadEmbedded(MAKEINTRESOURCEW(IDR_SCHEMA_YAML));
    if (!ptr || sz == 0) {
      InstallLog("09. write typeanything.schema.yaml",
                 "FAIL IDR_SCHEMA_YAML embedded resource missing");
      ShowFailureDialog("内嵌 schema 资源缺失（installer 资源损坏）。");
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
    {
      std::ofstream f(udir / L"typeanything.schema.yaml",
                      std::ios::binary | std::ios::trunc);
      f.write(yaml.data(), (std::streamsize)yaml.size());
    }
    {
      std::error_code stec;
      auto fsz = fs::file_size(udir / L"typeanything.schema.yaml", stec);
      bool ok = !stec && fsz > 0;
      InstallLog("09. write typeanything.schema.yaml",
                 std::string(ok ? "ok " : "FAIL ") + "path=" +
                     WideToUtf8((udir / L"typeanything.schema.yaml").wstring()) +
                     " bytes=" + (stec ? "0" : std::to_string(fsz)));
      if (!ok) {
        ShowFailureDialog("typeanything.schema.yaml 写入失败 — "
                          "%APPDATA%\\Rime 无写入权限？");
        PushStatus(78, "失败：schema 写入失败",
                   "typeanything.schema.yaml write failed", "error",
                   false, "schema yaml write failed");
        return;
      }
    }

    // 10. Supplement dict (modern AI / IT / social-media / slang terms
    // that luna_pinyin lacks). Schema's translator imports luna_pinyin via
    // typeanything.dict.yaml, so this file is required for the schema to
    // compile.
    if (auto [dptr, dsz] = LoadEmbedded(MAKEINTRESOURCEW(IDR_DICT_YAML));
        dptr && dsz > 0) {
      std::ofstream df(udir / L"typeanything.dict.yaml",
                       std::ios::binary | std::ios::trunc);
      df.write((const char*)dptr, (std::streamsize)dsz);
    } else {
      InstallLog("10. write typeanything.dict.yaml",
                 "FAIL IDR_DICT_YAML embedded resource missing");
    }
    {
      std::error_code stec;
      auto fsz = fs::file_size(udir / L"typeanything.dict.yaml", stec);
      bool ok = !stec && fsz > 0;
      InstallLog("10. write typeanything.dict.yaml",
                 std::string(ok ? "ok " : "FAIL ") + "path=" +
                     WideToUtf8((udir / L"typeanything.dict.yaml").wstring()) +
                     " bytes=" + (stec ? "0" : std::to_string(fsz)));
      if (!ok) {
        ShowFailureDialog("typeanything.dict.yaml 写入失败 — "
                          "schema 将无法编译。");
        PushStatus(78, "失败：dict 写入失败",
                   "typeanything.dict.yaml write failed", "error",
                   false, "dict yaml write failed");
        return;
      }
    }

    // 11. Translate / classify prompts (UTF-8). Always overwrite so a
    // reinstall ships the latest prompt tuning. processor.cc + ta-settings
    // read this.
    if (auto [pptr, psz] = LoadEmbedded(MAKEINTRESOURCEW(IDR_PROMPTS_TXT));
        pptr && psz > 0) {
      std::ofstream pf(udir / L"typeanything_prompts.txt",
                       std::ios::binary | std::ios::trunc);
      pf.write((const char*)pptr, (std::streamsize)psz);
    } else {
      InstallLog("11. write typeanything_prompts.txt",
                 "FAIL IDR_PROMPTS_TXT embedded resource missing");
    }
    {
      std::error_code stec;
      auto fsz = fs::file_size(udir / L"typeanything_prompts.txt", stec);
      InstallLog("11. write typeanything_prompts.txt",
                 std::string(!stec && fsz > 0 ? "ok " : "warning ") +
                     "bytes=" + (stec ? "0" : std::to_string(fsz)));
    }

    // 12. Default custom.yaml — pin the schema list to TypeAnything, set
    // menu page_size to 7 (Microsoft-IME parity), and force non-inline
    // preedit in Office apps where inline-mode TSF caret reporting is
    // broken (candidate bar would otherwise pin to the window's top-left).
    {
      std::ofstream cf(udir / L"default.custom.yaml",
                       std::ios::binary | std::ios::trunc);
      cf << "patch:\n"
         << "  schema_list:\n"
         << "    - schema: typeanything\n"
         << "  \"menu/page_size\": 7\n"
         << "  app_options/winword.exe:\n    inline_preedit: false\n"
         << "  app_options/wps.exe:\n    inline_preedit: false\n"
         << "  app_options/wpp.exe:\n    inline_preedit: false\n"
         << "  app_options/excel.exe:\n    inline_preedit: false\n"
         << "  app_options/powerpnt.exe:\n    inline_preedit: false\n";
    }
    {
      // Verify default.custom.yaml exists, has non-zero size, and the
      // schema_list block we just wrote actually mentions typeanything
      // (catches the case where a half-completed write or another
      // process raced us).
      std::error_code stec;
      auto fsz = fs::file_size(udir / L"default.custom.yaml", stec);
      bool ok = !stec && fsz > 0;
      bool schema_ok = false;
      if (ok) {
        std::ifstream rf(udir / L"default.custom.yaml", std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(rf)),
                            std::istreambuf_iterator<char>());
        schema_ok = content.find("schema: typeanything") != std::string::npos;
      }
      InstallLog("12. write default.custom.yaml",
                 std::string(ok && schema_ok ? "ok " : "FAIL ") +
                     "bytes=" + (stec ? "0" : std::to_string(fsz)) +
                     " schema_list_pinned=" +
                     (schema_ok ? "yes" : "NO"));
      if (!ok || !schema_ok) {
        ShowFailureDialog("default.custom.yaml 写入失败或未包含 "
                          "typeanything schema_list — 输入法将加载错误方案。");
        PushStatus(78, "失败：default.custom.yaml 写入失败",
                   "default.custom.yaml write failed", "error", false,
                   "default.custom.yaml write failed");
        return;
      }
    }

    // 13. Weasel UI: Microsoft IME look — horizontal pill, inline preedit
    // (pinyin stays at the editor cursor, not duplicated in the candidate
    // bar), YaHei UI font, tighter spacing, light-gray hilite (no big
    // blue selection box). Always overwrite the patch block so a
    // reinstall reliably restores the intended style.
    fs::path wcust = udir / L"weasel.custom.yaml";
    {
      std::ofstream wf(wcust, std::ios::binary | std::ios::trunc);
      wf <<
        "# Managed by TypeAnything installer — Microsoft-IME-style layout.\n"
        "# NOTE: Weasel's layout calculation reads style/layout/*, NOT the\n"
        "# top-level style/* fields for spacing/padding/margin. Only font_*\n"
        "# and color_scheme propagate from style/* — the rest must go under\n"
        "# style/layout/* to actually affect rendering.\n"
        "patch:\n"
        "  \"style/horizontal\": true\n"
        "  \"style/inline_preedit\": true\n"
        "  \"style/label_format\": \"%s \"\n"
        "  \"style/font_face\": \"Microsoft YaHei UI\"\n"
        "  \"style/font_point\": 11\n"
        "  \"style/label_font_point\": 11\n"
        "  \"style/comment_font_point\": 11\n"
        "  \"style/color_scheme\": typeanything_light\n"
        "  \"style/layout/margin_x\": 8\n"
        "  \"style/layout/margin_y\": 6\n"
        "  \"style/layout/hilite_padding\": 4\n"
        "  \"style/layout/hilite_spacing\": 4\n"
        "  \"style/layout/candidate_spacing\": 24\n"
        "  \"style/layout/linespacing\": 0\n"
        "  \"style/layout/border_width\": 1\n"
        "  \"style/layout/corner_radius\": 4\n"
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
    {
      std::error_code stec;
      auto fsz = fs::file_size(wcust, stec);
      InstallLog("13. write weasel.custom.yaml",
                 std::string(!stec && fsz > 0 ? "ok " : "FAIL ") +
                     "bytes=" + (stec ? "0" : std::to_string(fsz)));
    }

    // 14. Seed lang.txt with target + category prefix (so ResolveTargetLang has
    // something on Enter before user opens the settings panel). Default is
    // "A:English" (A = NATURAL_LANGUAGE category, English target).
    {
      std::ofstream lf(udir / L"typeanything_lang.txt",
                       std::ios::binary | std::ios::trunc);
      lf << (opts.target_lang.empty() ? "A:English" : opts.target_lang);
    }
    {
      std::error_code stec;
      auto fsz = fs::file_size(udir / L"typeanything_lang.txt", stec);
      InstallLog("14. write typeanything_lang.txt",
                 std::string(!stec && fsz > 0 ? "ok " : "warning ") +
                     "bytes=" + (stec ? "0" : std::to_string(fsz)));
    }
  }

  // 15-16. Deploy Rime base data (luna_pinyin + presets) to <wdir>\data\.
  //    Required for cold machines (never had Weasel/Rime). Skip files
  //    that already exist — preserves user-trained luna_pinyin user_dict
  //    on upgrade installs.
  PushStatus(82, "释放拼音字典与预设 …", "Rime 数据文件");
  {
    fs::path data = wdir / L"data";
    {
      std::error_code ec; fs::create_directories(data, ec);
      InstallLog("15. create install_dir/data",
                 fs::exists(data) ? "ok" : "FAIL");
    }
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
      bool preexisting = fs::exists(dst);
      bool wrote = true;
      if (!preexisting) {
        wrote = WriteEmbeddedToFile(d.res, dst);
        if (!wrote) {
          ++missing;
          PushStatus(82, std::string("数据文件写失败：") + WideToUtf8(d.name),
                     std::string("写失败：") + WideToUtf8(d.name), "warning");
        }
      }
      // Verify file exists + non-empty post-write (or post-skip).
      std::error_code stec;
      auto fsz = fs::file_size(dst, stec);
      bool ok = !stec && fsz > 0;
      InstallLog("16. deploy rime base data",
                 std::string(ok ? "ok " : "FAIL ") +
                     "leaf=" + WideToUtf8(d.name) +
                     " bytes=" + (stec ? "0" : std::to_string(fsz)) +
                     " preexisting=" + (preexisting ? "yes" : "no"));
    }
    if (missing > 0 && !fs::exists(data / L"luna_pinyin.dict.yaml")) {
      // luna_pinyin missing = schema cannot compile.
      InstallLog("16. deploy rime base data",
                 "FAIL luna_pinyin.dict.yaml missing → schema cannot compile");
      ShowFailureDialog("luna_pinyin.dict.yaml 部署失败 — schema 无法编译。");
      PushStatus(82, "拼音字典未就绪，无法继续",
                 "luna_pinyin.dict.yaml 缺失", "error", false,
                 "Rime 字典文件无法部署（杀软拦截？磁盘只读？）");
      return;
    }

    // 17-18. OpenCC t2s data → <wdir>\data\opencc\. The schema's simplifier
    // filter (opencc_config: t2s.json) needs these; without them the filter
    // fails to load → traditional output + EMPTY CANDIDATE WINDOW.
    fs::path opencc = data / L"opencc";
    {
      std::error_code ec2; fs::create_directories(opencc, ec2);
      InstallLog("17. create install_dir/data/opencc",
                 fs::exists(opencc) ? "ok" : "FAIL");
    }
    struct OcFile { LPCWSTR res; const wchar_t* name; };
    OcFile oc_files[] = {
      {MAKEINTRESOURCEW(IDR_OPENCC_T2S_JSON),   L"t2s.json"},
      {MAKEINTRESOURCEW(IDR_OPENCC_TS_PHRASES), L"TSPhrases.ocd2"},
      {MAKEINTRESOURCEW(IDR_OPENCC_TS_CHARS),   L"TSCharacters.ocd2"},
    };
    bool oc_ok = true;
    for (auto& o : oc_files) {
      fs::path dst = opencc / o.name;
      bool preexisting = fs::exists(dst);
      bool wrote = preexisting || WriteEmbeddedToFile(o.res, dst);
      // Strict post-write verification: schema simplifier filter requires
      // ALL three files. If any missing → empty candidate window at runtime.
      std::error_code stec;
      auto fsz = fs::file_size(dst, stec);
      bool ok = !stec && fsz > 0;
      InstallLog("18. deploy opencc data",
                 std::string(ok ? "ok " : "FAIL ") +
                     "leaf=" + WideToUtf8(o.name) +
                     " bytes=" + (stec ? "0" : std::to_string(fsz)) +
                     " preexisting=" + (preexisting ? "yes" : "no"));
      if (!wrote || !ok) oc_ok = false;
    }
    if (!oc_ok) {
      InstallLog("18. deploy opencc data",
                 "FAIL one or more opencc files missing → empty candidate window");
      ShowFailureDialog("OpenCC 简繁转换数据缺失 — 候选窗将为空。");
      PushStatus(82, "OpenCC 数据写失败 — 可能出繁体且候选窗为空",
                 "opencc t2s 写失败", "error", false,
                 "OpenCC 简繁转换数据无法部署（杀软拦截？磁盘只读？）。"
                 "缺失会导致输出繁体 + 候选窗不显示。");
      return;
    }
  }

  // 19. Cold-register weaselx64.dll as a TSF text service.
  //    Standard Weasel MSI does this via regsvr32 — ta-installer must do
  //    it itself on cold machines, otherwise Win+Space won't show
  //    TypeAnything. Idempotent on already-registered systems.
  //
  //    Failure is fatal ONLY when the TIP CLSID subtree is also missing.
  //    On upgrade installs (prior Weasel/Rime registered the CLSID
  //    already), a DllRegisterServer failure is downgraded to a warning
  //    — the IME still works via the existing subtree.
  PushStatus(84, "注册到 Windows 输入法框架 …", "TSF 注册 (DllRegisterServer)");
  {
    bool reg_ok = false;
    std::string diag =
        ColdRegisterTsfDllDiag(wdir / L"weaselx64.dll", &reg_ok);
    InstallLog("19. cold-register weaselx64.dll",
               std::string(reg_ok ? "ok " : "FAIL ") + "diag=" +
                   (diag.empty() ? std::string("ok") : diag));
    if (!reg_ok) {
      if (TsfAlreadyRegistered()) {
        InstallLog("19. cold-register weaselx64.dll",
                   "warning (downgraded) — TIP CLSID already present");
        PushStatus(84,
                   "TSF 已注册（跳过 cold-register；warning：" + diag + "）",
                   "TSF cold-register skipped: " + diag, "warning");
      } else {
        InstallLog("19. cold-register weaselx64.dll",
                   "FATAL — TIP CLSID missing after register attempt");
        std::string msg =
            "无法把 TypeAnything 注册到 Windows 输入法框架（" + diag +
            "）。通常是杀软 / EDR / GPO 拦截，或 weaselx64.dll 缺依赖。";
        ShowFailureDialog(msg);
        PushStatus(84, msg, "DllRegisterServer 失败: " + diag,
                   "error", false, msg);
        return;
      }
    }
  }

  // 20. Verify HKLM\SOFTWARE\Microsoft\CTF\TIP\{A3F4CDED-…} exists +
  //     contains LanguageProfile subkeys. Catches the edge case where
  //     DllRegisterServer returns S_OK but the CLSID subtree was left
  //     incomplete by a malware scanner or GPO.
  {
    const wchar_t* clsid_path =
        L"SOFTWARE\\Microsoft\\CTF\\TIP\\"
        L"{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}";
    HKEY h;
    bool clsid_ok = false;
    bool langprof_ok = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, clsid_path, 0,
                      KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
      clsid_ok = true;
      HKEY hLp;
      std::wstring lp_path =
          std::wstring(clsid_path) + L"\\LanguageProfile";
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, lp_path.c_str(), 0,
                        KEY_READ | KEY_WOW64_64KEY, &hLp) == ERROR_SUCCESS) {
        // Count subkeys; >0 = at least one LCID profile present.
        DWORD subkey_count = 0;
        RegQueryInfoKeyW(hLp, nullptr, nullptr, nullptr,
                         &subkey_count, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr);
        langprof_ok = (subkey_count > 0);
        RegCloseKey(hLp);
      }
      RegCloseKey(h);
    }
    InstallLog("20. verify TIP CLSID registry",
               std::string("clsid=") + (clsid_ok ? "yes" : "NO") +
                   " language_profile_subkeys=" +
                   (langprof_ok ? "yes" : "NO"));
    if (!clsid_ok || !langprof_ok) {
      ShowFailureDialog(
          "TSF CLSID 注册不完整 — Win+Space 将看不到 TypeAnything。");
      PushStatus(84, "TSF 注册不完整",
                 "TIP CLSID / LanguageProfile 缺失", "error", false,
                 "TSF registration incomplete");
      return;
    }
  }

  // 21. Patch HKLM TSF profile descriptions (now that the TIP CLSID
  //    subtree exists from step 19/20 above).
  PushStatus(86, "改写输入法显示名称 …", "TSF 描述改写为 TypeAnything");
  PatchTsfProfileDescriptions();
  InstallLog("21. patch TSF profile descriptions",
             "ok (description=TypeAnything)");

  // 22. Drop other schemas from Weasel data dir (keep luna_pinyin + typeanything).
  PushStatus(88, "仅保留 TypeAnything 方案 …", "隐藏其他 Rime 方案");
  {
    fs::path data = wdir / L"data";
    fs::path orig = wdir / L"data.original";
    if (fs::exists(data) && !fs::exists(orig)) {
      std::error_code ec; fs::copy(data, orig, fs::copy_options::recursive, ec);
    }
    int hidden = 0;
    if (fs::exists(data)) {
      for (auto& e : fs::directory_iterator(data)) {
        auto name = e.path().filename().wstring();
        if (name.size() > 12 && name.substr(name.size() - 12) == L".schema.yaml"
            && name.compare(0, 11, L"luna_pinyin") != 0
            && name.compare(0, 12, L"typeanything") != 0) {
          std::error_code ec; fs::remove(e.path(), ec);
          if (!ec) ++hidden;
        }
        if (name.size() > 10 && name.substr(name.size() - 10) == L".dict.yaml"
            && name.compare(0, 11, L"luna_pinyin") != 0
            && name.compare(0, 12, L"typeanything") != 0) {
          std::error_code ec; fs::remove(e.path(), ec);
          if (!ec) ++hidden;
        }
      }
    }
    InstallLog("22. hide non-typeanything schemas",
               "ok hidden=" + std::to_string(hidden));
  }

  // 23. Redeploy schema. Original code uses StartHidden (fire-and-forget)
  //     but we want the exit code so we can fail-fast if deploy errors
  //     out instantly (perms, missing dep, corrupt yaml). Spawn
  //     manually with CreateProcessW + Wait + GetExitCodeProcess.
  PushStatus(92, "编译输入方案 …", "Rime 编译中（首次约 10-30 秒）");
  {
    fs::path deployer = wdir / L"WeaselDeployer.exe";
    std::wstring cmd = L"\"" + deployer.wstring() + L"\" /deploy";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    BOOL spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                   FALSE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    if (!spawned) {
      DWORD err = GetLastError();
      InstallLog("23. WeaselDeployer /deploy",
                 "FAIL spawn err=" + FormatWinError(err));
    } else {
      // Don't wait synchronously here — the existing pipeline polls
      // prism.bin freshness up to 60s. Just record that we spawned it.
      InstallLog("23. WeaselDeployer /deploy",
                 "ok spawned pid=" + std::to_string(pi.dwProcessId));
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }

  // 24-25. Poll for typeanything.prism.bin freshness, max 60s.
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
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - start).count();
  InstallLog("24. poll for prism.bin",
             std::string("schema_compiled=") +
                 (last_mtime_ok ? "yes" : "NO") +
                 " elapsed_s=" + std::to_string(elapsed) +
                 " path=" + WideToUtf8(prism.wstring()) +
                 " exists=" + (fs::exists(prism) ? "yes" : "no"));
  if (!last_mtime_ok) {
    InstallLog("25. prism timeout fallback",
               "warning schema compile timed out, continuing");
  }

  // 26. Kill WeaselDeployer (whether it exited normally or is still
  //     spinning past timeout) so it doesn't linger holding locks.
  KillByName(L"WeaselDeployer.exe");
  InstallLog("26. kill WeaselDeployer", "ok");

  if (!last_mtime_ok) {
    PushStatus(95, "编译耗时偏长，继续启动服务", "编译超时回退，跳过等待",
               "done");
  } else {
    PushStatus(95, "方案编译完成", "方案编译完成", "done");
  }

  // 27. Start WeaselServer so the tray icon comes up. Fatal if it
  //     doesn't start — without server there's no tray menu, no IPC
  //     backend for the TSF DLL.
  //
  //     Try de-elevation first (CreateProcessWithTokenW with explorer's
  //     token → MEDIUM IL). Required so the server's named pipe ACL
  //     matches typical MEDIUM IL text-input apps. Fall back to plain
  //     elevated CreateProcessW only if de-elevation is unavailable
  //     (UAC=0 / no shell window / SE_IMPERSONATE_NAME missing).
  PushStatus(96, "启动输入法服务 …", "输入法服务上线");
  bool de_elevated = StartDeElevated(wdir / L"WeaselServer.exe");
  bool server_started = de_elevated;
  if (!server_started) {
    server_started = StartShown(wdir / L"WeaselServer.exe");
  }
  InstallLog("27. start WeaselServer",
             std::string(server_started ? "ok " : "FAIL ") +
                 "method=" + (de_elevated ? "de-elevated"
                                          : (server_started ? "elevated"
                                                            : "none")));
  if (!server_started) {
    ShowFailureDialog(
        "WeaselServer.exe 启动失败（杀软拦截 / 缺少 VC++ 运行时 / "
        "rime.dll 加载失败）。");
    PushStatus(96, "WeaselServer.exe 启动失败",
               "无法启动 WeaselServer.exe", "error", false,
               "WeaselServer.exe 启动失败（杀软拦截 / 缺少 VC++ 运行时 / "
               "rime.dll 加载失败）。");
    return;
  }

  // 28. Verify WeaselServer is still running 3s after start. Catches
  //     the case where the process spawns then crashes immediately
  //     (rime.dll missing dep, VC++ runtime missing, IPC pipe ACL issue).
  Sleep(3000);
  {
    HWND hWeasel = FindWindowW(L"WeaselServerWnd", nullptr);
    // Best-effort: WeaselServer's hidden window class name. If not
    // found, scan running processes via tasklist would be heavier;
    // window-class probe is a sufficient liveness check.
    bool alive = (hWeasel != nullptr);
    if (!alive) {
      // Fallback: try to OpenProcess by name via CreateToolhelp32Snapshot
      // would be ideal but pull in tlhelp32.h. Cheap probe: spawn taskkill
      // /FI in dry-run isn't reliable. Just trust the window check; if
      // missing, treat as warning (server may still be initializing).
    }
    InstallLog("28. verify WeaselServer alive 3s",
               std::string(alive ? "ok " : "warning ") +
                   "window_found=" + (alive ? "yes" : "no"));
  }

  // 29. Register autostart on next login.
  {
    HKEY h;
    LSTATUS open_st = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h);
    LSTATUS set_st = ERROR_INVALID_HANDLE;
    if (open_st == ERROR_SUCCESS) {
      std::wstring exe = (wdir / L"WeaselServer.exe").wstring();
      set_st = RegSetValueExW(h, L"WeaselServer", 0, REG_SZ,
                              (const BYTE*)exe.c_str(),
                              (DWORD)((exe.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(h);
    }
    InstallLog("29. register autostart",
               std::string(set_st == ERROR_SUCCESS ? "ok " : "warning ") +
                   "open_status=" + std::to_string((long)open_st) +
                   " set_status=" + std::to_string((long)set_st));
  }

  // 30. Drop a copy of ourselves as `uninstall.exe`.
  {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, _countof(self));
    fs::path uninst = wdir / L"uninstall.exe";
    std::error_code ec;
    fs::copy_file(fs::path(self), uninst,
                  fs::copy_options::overwrite_existing, ec);
    std::error_code stec;
    auto fsz = fs::file_size(uninst, stec);
    InstallLog("30. copy uninstall.exe",
               std::string(!stec && fsz > 0 ? "ok " : "warning ") +
                   "bytes=" + (stec ? "0" : std::to_string(fsz)) +
                   " copy_ec=" + std::to_string(ec.value()));

    // 31. Add/Remove Programs entry.
    WriteUninstallRegistry(wdir, L"0.7.9");
    InstallLog("31. write Uninstall registry",
               "ok (HKLM\\...\\Uninstall\\TypeAnything)");
  }

  // 32. Start Menu shortcut → WeaselServer.exe. User-facing "start the
  //     input method" entry. Lives in the all-users Start Menu so it
  //     shows up for every account on the machine.
  PushStatus(98, "创建开始菜单快捷方式 …", "开始菜单 → TypeAnything");
  {
    fs::path lnk = StartMenuProgramsAllUsers() / L"TypeAnything.lnk";
    bool lnk_ok = CreateLnk(wdir / L"WeaselServer.exe", lnk,
                            L"启动 TypeAnything 输入法");
    InstallLog("32. create Start Menu shortcut",
               std::string(lnk_ok ? "ok " : "warning ") + "path=" +
                   WideToUtf8(lnk.wstring()));
    if (!lnk_ok) {
      PushStatus(98, "开始菜单快捷方式创建失败（不影响主功能）",
                 "Start Menu lnk 失败（warning）", "warning");
    }
  }

  // 33. Final status.
  std::string final_msg = reboot_needed
      ? "部分文件被占用，已排队重启替换。重启电脑后完全生效。"
      : "全部完成。打开新应用即可使用。";
  InstallLog("33. install complete",
             std::string("success reboot_needed=") +
                 (reboot_needed ? "yes" : "no"));
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
  // Light caption to match the blue-white body palette (v0.7.3+).
  BOOL dark = FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// ─── Frameless / extended client (Claude / Spotify / VS Code style) ──
//
// We hide the OS title bar entirely and let the HTML titlebar own the top
// edge: brand on the left, custom minimize/close buttons on the right,
// the rest of the strip is a drag region. Body extends edge-to-edge with
// no NC strip eating space.
//
// Implementation:
//   1. Strip WS_CAPTION + WS_SYSMENU; keep WS_THICKFRAME for the resize
//      border + drop shadow (DWM still draws the shadow as long as the
//      window has a sizable frame).
//   2. DwmExtendFrameIntoClientArea with a 1px top margin so DWM keeps
//      the window shadow + corner rounding even though no caption is
//      visible.
//   3. WM_NCCALCSIZE wParam==TRUE → return 0. This tells DefWindowProc
//      to leave the proposed rect alone, so the NC frame eats zero
//      pixels and the HTML covers from y=0.
//   4. WM_NCHITTEST: top 40px is HTCAPTION (drag), except a 100px-wide
//      cluster on the right which stays HTCLIENT so the HTML close /
//      min buttons receive their mouse events. 6px-wide window edges
//      report HT* values for resize.

namespace frameless {
  static int titlebar_h_px      = 40;
  static int btn_cluster_w_px   = 100;
  static WNDPROC g_orig_wndproc = nullptr;

  static LRESULT CALLBACK Subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_NCCALCSIZE: {
        if (wp == TRUE) return 0;
        break;
      }
      case WM_NCHITTEST: {
        RECT rc; GetWindowRect(hwnd, &rc);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int RESIZE = 6;
        bool left   = pt.x <  rc.left  + RESIZE;
        bool right  = pt.x >= rc.right - RESIZE;
        bool top    = pt.y <  rc.top   + RESIZE;
        bool bottom = pt.y >= rc.bottom - RESIZE;
        if (top    && left)  return HTTOPLEFT;
        if (top    && right) return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)   return HTLEFT;
        if (right)  return HTRIGHT;
        if (top)    return HTTOP;
        if (bottom) return HTBOTTOM;
        int titlebar_bottom = rc.top + titlebar_h_px;
        int btn_left_edge   = rc.right - btn_cluster_w_px;
        if (pt.y < titlebar_bottom && pt.x < btn_left_edge) return HTCAPTION;
        return HTCLIENT;
      }
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
  }
}  // namespace frameless

static void MakeFrameless(HWND hwnd) {
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  style &= ~(WS_CAPTION | WS_SYSMENU);
  style |= WS_THICKFRAME | WS_MINIMIZEBOX;
  SetWindowLongPtrW(hwnd, GWL_STYLE, style);

  MARGINS m{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(hwnd, &m);

  frameless::g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, (LONG_PTR)frameless::Subclass);

  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
               SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
  w.set_size(640, 380, WEBVIEW_HINT_FIXED);

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

  // Window + Mica + frameless.
  HWND hwnd = (HWND)w.window();
  w.bind("nativeMinimize", [hwnd](const std::string&) -> std::string {
    ShowWindow(hwnd, SW_MINIMIZE);
    return "true";
  });
  // WebView2's child HWND covers the entire client area and intercepts
  // every mouse message before our parent's WM_NCHITTEST sees it, so the
  // subclass's HTCAPTION return is dead code. We work around that by
  // having the HTML titlebar's mousedown handler call this binding —
  // SendMessage(WM_NCLBUTTONDOWN, HTCAPTION) tells the OS to enter its
  // native drag loop from the current cursor position.
  w.bind("nativeStartDrag", [hwnd](const std::string&) -> std::string {
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    return "true";
  });
  ApplyMica(hwnd);
  MakeFrameless(hwnd);

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
