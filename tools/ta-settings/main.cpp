// TypeAnything Settings — modern WebView2 + HTML/CSS UI for the two tray menu
// items "Switch Language" and "Model Config". Replaces the old PowerShell
// InputBox / WinForms popups.
//
// Usage:
//   ta-settings.exe --page lang     (default if no arg)
//   ta-settings.exe --page model
//
// Reads / writes:
//   %APPDATA%\Rime\typeanything_lang.txt
//   %APPDATA%\Rime\typeanything.schema.yaml
//
// JS bridge functions exposed to the page:
//   nativeReadLang()                            → string
//   nativeWriteLang(text)                       → void
//   nativeReadSchema()                          → {api_key, model, host, path}
//   nativeWriteSchema(api_key, model, host, path) → void  (+ kicks deployer + server restart)
//   nativeOpenUrl(url)                          → void
//   nativeClose()                               → void

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <winhttp.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "vendor/webview.h"

namespace fs = std::filesystem;

// ─── Single-instance / cross-process IPC ─────────────────────────
// When the user clicks a tray menu while ta-settings is already open we want
// to (1) NOT spawn a second window and (2) raise the existing one + switch
// the visible page. Implementation: named mutex for single-instance check, a
// named file mapping to share the HWND across processes, and a custom
// WM_APP+1 message PostMessage'd from the secondary instance.

static const wchar_t* TA_MUTEX_NAME = L"Local\\TypeAnything-ta-settings-singleton-v1";
static const wchar_t* TA_HWND_MAP_NAME = L"Local\\TypeAnything-ta-settings-hwnd-v1";
// wparam: 0 = lang, 1 = model
static const UINT WM_TA_SHOWPAGE = WM_APP + 1;

// ───────────────────────── helpers ──────────────────────────

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

static fs::path RimeUserDir() {
  PWSTR p = nullptr;
  std::wstring root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) {
    root = p;
    CoTaskMemFree(p);
  }
  return fs::path(root) / L"Rime";
}

static fs::path LangFilePath()    { return RimeUserDir() / L"typeanything_lang.txt"; }
static fs::path SchemaFilePath()  { return RimeUserDir() / L"typeanything.schema.yaml"; }
// Per-provider API key store. JSON map { "deepseek": "sk-...", ... } so
// switching presets doesn't carry over the wrong vendor's key into another
// vendor's field. schema.yaml still holds the *active* provider's key for
// the runtime; this is just a UI memory.
static fs::path KeyringFilePath() { return RimeUserDir() / L"typeanything.keyring.json"; }
static fs::path PromptsFilePath() { return RimeUserDir() / L"typeanything_prompts.txt"; }

static std::string ReadAllUtf8(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f.is_open()) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  // strip UTF-8 BOM if present
  if (s.size() >= 3 && (unsigned char)s[0] == 0xEF
      && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
    s.erase(0, 3);
  }
  return s;
}

static bool WriteAllUtf8(const fs::path& p, const std::string& content) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;
  f.write(content.data(), (std::streamsize)content.size());
  return f.good();
}

// Read the ===<section>=== body from typeanything_prompts.txt (UTF-8).
static std::string ReadPromptSection(const std::string& section) {
  std::string raw = ReadAllUtf8(PromptsFilePath());
  if (raw.empty()) return "";
  std::string marker = "===" + section + "===";
  size_t start = raw.find(marker);
  if (start == std::string::npos) return "";
  start += marker.size();
  if (start < raw.size() && raw[start] == '\r') ++start;
  if (start < raw.size() && raw[start] == '\n') ++start;
  size_t end = raw.find("\n===", start);
  std::string body =
      (end == std::string::npos) ? raw.substr(start) : raw.substr(start, end - start);
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' ||
                           body.back() == ' ' || body.back() == '\t'))
    body.pop_back();
  return body;
}

static std::string ReplaceAllStr(std::string s, const std::string& a,
                                 const std::string& b) {
  if (a.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(a, pos)) != std::string::npos) {
    s.replace(pos, a.size(), b);
    pos += b.size();
  }
  return s;
}

// HTTPS POST to host+path with Bearer api_key. Returns response body (UTF-8)
// or "" on transport failure; sets *err to a human message on failure.
static std::string HttpsPost(const std::string& host, const std::string& path,
                             const std::string& api_key,
                             const std::string& body, std::string* err) {
  auto fail = [&](const std::string& m) -> std::string {
    if (err) *err = m;
    return "";
  };
  // host may be "api.deepseek.com" or include scheme — strip scheme.
  std::string h = host;
  size_t sp = h.find("://");
  if (sp != std::string::npos) h = h.substr(sp + 3);
  // strip any path that snuck into host
  size_t slash = h.find('/');
  if (slash != std::string::npos) h = h.substr(0, slash);
  std::wstring whost = Utf8ToWide(h);
  std::wstring wpath = Utf8ToWide(path.empty() ? "/v1/chat/completions" : path);

  // NO_PROXY default access — AUTOMATIC_PROXY does WPAD auto-detect which
  // can hang for many seconds on some networks. We talk to a public HTTPS
  // endpoint; if a corporate proxy is required the user's WinINET settings
  // still apply via the system default, but we avoid the WPAD stall.
  HINTERNET hSession = WinHttpOpen(L"TypeAnything-Classifier/1.0",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) return fail("WinHttpOpen failed");
  // Bounded timeouts (ms): resolve / connect / send / receive. Prevents the
  // call from hanging forever (the "分类中…" stuck-forever bug).
  WinHttpSetTimeouts(hSession, 5000, 8000, 10000, 15000);
  HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(),
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) { WinHttpCloseHandle(hSession); return fail("WinHttpConnect failed"); }
  HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath.c_str(), NULL,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE);
  if (!hReq) {
    WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return fail("WinHttpOpenRequest failed");
  }
  std::wstring headers =
      L"Content-Type: application/json\r\nAuthorization: Bearer " +
      Utf8ToWide(api_key);
  BOOL ok = WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1L,
                               (LPVOID)body.data(), (DWORD)body.size(),
                               (DWORD)body.size(), 0);
  if (ok) ok = WinHttpReceiveResponse(hReq, NULL);
  std::string resp;
  if (ok) {
    DWORD avail = 0;
    do {
      avail = 0;
      if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
      std::string chunk(avail, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(hReq, &chunk[0], avail, &read)) break;
      chunk.resize(read);
      resp += chunk;
    } while (avail > 0);
  }
  DWORD status = 0, slen = sizeof(status);
  WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                      WINHTTP_NO_HEADER_INDEX);
  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  if (!ok) return fail("网络请求失败（检查网络 / 防火墙）");
  if (status == 401) return fail("HTTP 401 — API key 无效或未配置");
  if (status >= 400) return fail("HTTP " + std::to_string(status));
  return resp;
}

// Pull the assistant message content out of an OpenAI-compatible JSON reply.
static std::string ExtractContent(const std::string& json) {
  size_t k = json.find("\"content\"");
  if (k == std::string::npos) return "";
  size_t colon = json.find(':', k);
  if (colon == std::string::npos) return "";
  size_t q1 = json.find('"', colon + 1);
  if (q1 == std::string::npos) return "";
  std::string out;
  for (size_t i = q1 + 1; i < json.size(); ++i) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.size()) {
      char e = json[++i];
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'u': {
          if (i + 4 < json.size()) {
            unsigned cp = (unsigned)strtol(json.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp < 0x80) out.push_back((char)cp);
            else if (cp < 0x800) {
              out.push_back((char)(0xC0 | (cp >> 6)));
              out.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
              out.push_back((char)(0xE0 | (cp >> 12)));
              out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back((char)(0x80 | (cp & 0x3F)));
            }
          }
          break;
        }
        default: out.push_back(e); break;
      }
    } else if (c == '"') {
      break;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// ─── JSON helpers (handcrafted; no library dep) ──────────────

static std::string JsonEscape(const std::string& s) {
  std::string r;
  r.reserve(s.size() + 8);
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

// Parse a single JSON string argument out of webview-bind's args array.
// args looks like:  ["the string"]   or   ["s1","s2",...]
static std::vector<std::string> ParseJsonStringArray(const std::string& json) {
  std::vector<std::string> out;
  size_t i = 0;
  auto skipWs = [&]() { while (i < json.size() && isspace((unsigned char)json[i])) ++i; };
  skipWs();
  if (i >= json.size() || json[i] != '[') return out;
  ++i;
  while (i < json.size()) {
    skipWs();
    if (json[i] == ']') break;
    if (json[i] != '"') break;
    ++i;
    std::string s;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) {
        char esc = json[i + 1];
        switch (esc) {
          case 'n': s.push_back('\n'); break;
          case 'r': s.push_back('\r'); break;
          case 't': s.push_back('\t'); break;
          case '"': s.push_back('"'); break;
          case '\\': s.push_back('\\'); break;
          case '/': s.push_back('/'); break;
          case 'u': {
            if (i + 5 < json.size()) {
              unsigned cp = 0;
              for (int k = 0; k < 4; ++k) {
                char h = json[i + 2 + k];
                cp <<= 4;
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
          default: s.push_back(esc); break;
        }
        i += 2;
      } else {
        s.push_back(json[i]);
        ++i;
      }
    }
    out.push_back(std::move(s));
    if (i < json.size()) ++i;       // skip closing "
    skipWs();
    if (i < json.size() && json[i] == ',') ++i;
  }
  return out;
}

// ─── Schema yaml field accessors ─────────────────────────────

// Find first line whose trimmed-left form looks like `<key>:` and return
// the value to the right (with surrounding quotes/whitespace stripped).
// Plain string scan — MSVC's std::regex flavor doesn't understand the
// `(?m)` inline modifier and throws std::regex_error, which is why the
// earlier regex-based version crashed the model page on launch.
static std::string ExtractYamlField(const std::string& yaml, const std::string& key) {
  size_t i = 0;
  while (i < yaml.size()) {
    size_t eol = yaml.find('\n', i);
    if (eol == std::string::npos) eol = yaml.size();
    std::string line = yaml.substr(i, eol - i);
    i = eol + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t lead = 0;
    while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
    if (lead + key.size() + 1 > line.size()) continue;
    if (line.compare(lead, key.size(), key) != 0) continue;
    if (line[lead + key.size()] != ':') continue;
    // Skip whitespace after colon.
    size_t v = lead + key.size() + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    std::string val = line.substr(v);
    // Strip wrapping double quotes.
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }
    return val;
  }
  return "";
}

// Sanitize a value before splicing it into a YAML scalar. Strips line
// breaks (no field should ever contain newlines — they would inject new
// YAML keys downstream); for the quoted form also escapes the chars that
// would close the double-quoted scalar.
static std::string SanitizeYamlValue(const std::string& v, bool quoted) {
  std::string r;
  r.reserve(v.size());
  for (char c : v) {
    if (c == '\n' || c == '\r') continue;  // drop line breaks
    if (quoted) {
      if (c == '\\' || c == '"') r.push_back('\\');
    }
    r.push_back(c);
  }
  return r;
}

// Replace the value portion of `<indent><key>:<spaces>...` on the first
// matching line. Quoted controls whether the new value is wrapped in "".
static std::string ReplaceYamlField(const std::string& yaml,
                                    const std::string& key,
                                    const std::string& val,
                                    bool quoted) {
  std::string out;
  out.reserve(yaml.size() + val.size() + 8);
  size_t i = 0;
  bool replaced = false;
  while (i < yaml.size()) {
    size_t eol = yaml.find('\n', i);
    bool has_nl = (eol != std::string::npos);
    if (!has_nl) eol = yaml.size();
    std::string line = yaml.substr(i, eol - i);
    bool did = false;
    if (!replaced) {
      size_t lead = 0;
      while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
      if (lead + key.size() + 1 <= line.size()
          && line.compare(lead, key.size(), key) == 0
          && line[lead + key.size()] == ':') {
        size_t v = lead + key.size() + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        std::string head = line.substr(0, v);
        std::string sval = SanitizeYamlValue(val, quoted);
        std::string new_val = quoted ? "\"" + sval + "\"" : sval;
        out += head + new_val;
        replaced = true;
        did = true;
      }
    }
    if (!did) out += line;
    if (has_nl) out += '\n';
    i = eol + 1;
  }
  return out;
}

// ─── Server restart after schema change ──────────────────────

static fs::path WeaselDir() {
  // HKLM\Software\Rime\Weasel:WeaselRoot
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Rime\\Weasel", 0,
                    KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
    WCHAR buf[MAX_PATH] = {0};
    DWORD len = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, L"WeaselRoot", nullptr, &type,
                         (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ) {
      RegCloseKey(hKey);
      return fs::path(buf);
    }
    RegCloseKey(hKey);
  }
  return fs::path(L"C:\\Program Files\\Rime\\weasel-0.17.4");
}

static void KillProcByName(const wchar_t* image) {
  // No /T — that kills the process tree. WeaselServer is the parent of
  // ta-settings (via ShellExecute / CreateProcess from the tray menu), so
  // /T would take this very settings panel down with it.
  std::wstring cmd = L"taskkill /F /IM ";
  cmd += image;
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

static void RedeployAndRestartServer() {
  fs::path dir = WeaselDir();
  KillProcByName(L"WeaselServer.exe");
  Sleep(300);
  fs::path deployer = dir / L"WeaselDeployer.exe";
  fs::path server   = dir / L"WeaselServer.exe";
  if (fs::exists(deployer)) {
    StartHidden(deployer, L"/deploy");
    Sleep(3000);
    KillProcByName(L"WeaselDeployer.exe");
  }
  if (fs::exists(server)) {
    StartHidden(server);
  }
}

// ─── Locate the bundled UI directory ─────────────────────────

static fs::path ExeDir() {
  wchar_t buf[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).remove_filename();
}

static std::string FileUrl(const fs::path& p) {
  std::wstring w = p.wstring();
  // Replace backslashes with forward slashes, prepend file:///
  for (auto& c : w) if (c == L'\\') c = L'/';
  return "file:///" + WideToUtf8(w);
}

// ─── Page argument parsing ───────────────────────────────────

static std::string GetPageArg() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string page = "lang";
  if (argv) {
    for (int i = 1; i < argc - 1; ++i) {
      if (std::wstring(argv[i]) == L"--page") {
        page = WideToUtf8(argv[i + 1]);
        break;
      }
    }
    LocalFree(argv);
  }
  if (page != "lang" && page != "model") page = "lang";
  return page;
}

// ─── Mica acrylic on the host window ─────────────────────────

static void ApplyMica(HWND hwnd) {
  // Win11 only. Best-effort; ignored on older Windows.
  enum DWMSBT { DWMSBT_AUTO = 0, DWMSBT_NONE, DWMSBT_MAINWINDOW = 2,
                DWMSBT_TRANSIENTWINDOW, DWMSBT_TABBEDWINDOW };
  const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
  const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
  int backdrop = DWMSBT_MAINWINDOW;
  DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

  // Match dark mode to system
  HKEY k;
  BOOL useLight = TRUE;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    0, KEY_READ, &k) == ERROR_SUCCESS) {
    DWORD v = 1, len = sizeof(v), type = 0;
    if (RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type,
                         (LPBYTE)&v, &len) == ERROR_SUCCESS) {
      useLight = v ? TRUE : FALSE;
    }
    RegCloseKey(k);
  }
  BOOL dark = useLight ? FALSE : TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// ───────────────────────── main ──────────────────────────────

// Subclass state (only one ta-settings window per process; module-level OK).
static WNDPROC g_original_wndproc = nullptr;
static webview::webview* g_webview = nullptr;

static LRESULT CALLBACK TaSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TA_SHOWPAGE) {
    const char* pg = (wp == 1) ? "model" : "lang";
    if (g_webview) {
      // Run on the UI thread via webview::dispatch — eval + foreground
      // must touch the webview / HWND from the message-pump thread.
      g_webview->dispatch([pg, hwnd]() {
        if (g_webview) {
          std::string js =
              "if (typeof showPage === 'function') showPage('";
          js += pg;
          js += "');";
          g_webview->eval(js);
        }
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        else                ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
      });
    }
    return 0;
  }
  return CallWindowProcW(g_original_wndproc, hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  std::string page = GetPageArg();

  // Single-instance check. If another ta-settings is already running,
  // post it a WM_TA_SHOWPAGE with the requested page and exit. The
  // existing window will switch tabs + raise itself.
  HANDLE hMutex = CreateMutexW(nullptr, FALSE, TA_MUTEX_NAME);
  DWORD mutex_err = GetLastError();
  if (mutex_err == ERROR_ALREADY_EXISTS) {
    HANDLE hMapR = OpenFileMappingW(FILE_MAP_READ, FALSE, TA_HWND_MAP_NAME);
    HWND existing = nullptr;
    if (hMapR) {
      HWND* p = (HWND*)MapViewOfFile(hMapR, FILE_MAP_READ, 0, 0, sizeof(HWND));
      if (p) { existing = *p; UnmapViewOfFile(p); }
      CloseHandle(hMapR);
    }
    if (existing && IsWindow(existing)) {
      DWORD owner_pid = 0;
      GetWindowThreadProcessId(existing, &owner_pid);
      if (owner_pid) AllowSetForegroundWindow(owner_pid);
      WPARAM page_id = (page == "model") ? 1 : 0;
      PostMessageW(existing, WM_TA_SHOWPAGE, page_id, 0);
      if (hMutex) CloseHandle(hMutex);
      return 0;
    }
    // Stale mutex (previous process crashed): fall through and own it
    // ourselves on the next CreateMutex iteration. The mutex is now
    // un-named-but-orphaned; close + re-create to claim ownership.
    if (hMutex) CloseHandle(hMutex);
    hMutex = CreateMutexW(nullptr, TRUE, TA_MUTEX_NAME);
  }

  // WebView2 needs a writable user-data folder. Its default is alongside
  // the exe, which is C:\Program Files\Rime\weasel-0.17.4 (read-only).
  // Force it under %LOCALAPPDATA%\TypeAnything\WebView2 instead.
  {
    wchar_t lad[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, lad))) {
      std::wstring ud = std::wstring(lad) + L"\\TypeAnything\\WebView2";
      std::error_code ec;
      fs::create_directories(ud, ec);
      SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", ud.c_str());
    }
  }

  // Webview window (Edge WebView2).
  webview::webview w(true /* debug */, nullptr);
  w.set_title("TypeAnything");
  // Same dimensions for both pages — user switches between them via the
  // top tab nav, so the window size shouldn't jump.
  w.set_size(700, 810, WEBVIEW_HINT_NONE);
  w.set_size(560, 600, WEBVIEW_HINT_MIN);

  // ─── Native bridge ────────────────────────────────────────
  w.bind("nativeReadLang", [](const std::string& /*args*/) -> std::string {
    std::string raw = ReadAllUtf8(LangFilePath());
    // First non-empty, non-comment line
    std::string out;
    std::stringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '
                               || line.back() == '\t')) line.pop_back();
      size_t lead = 0;
      while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
      std::string t = line.substr(lead);
      if (t.empty() || t[0] == '#') continue;
      out = t;
      break;
    }
    return "\"" + JsonEscape(out) + "\"";
  });

  w.bind("nativeWriteLang", [](const std::string& args) -> std::string {
    auto parts = ParseJsonStringArray(args);
    if (parts.empty()) return "false";
    bool ok = WriteAllUtf8(LangFilePath(), parts[0]);
    return ok ? "true" : "false";
  });

  w.bind("nativeReadSchema", [](const std::string& /*args*/) -> std::string {
    std::string yaml = ReadAllUtf8(SchemaFilePath());
    std::string apikey = ExtractYamlField(yaml, "api_key");
    std::string model  = ExtractYamlField(yaml, "model");
    std::string host   = ExtractYamlField(yaml, "host");
    std::string path   = ExtractYamlField(yaml, "path");
    if (model.empty())  model = "deepseek-chat";
    if (host.empty())   host  = "api.deepseek.com";
    if (path.empty())   path  = "/v1/chat/completions";
    std::ostringstream j;
    j << "{"
      << "\"api_key\":\"" << JsonEscape(apikey) << "\","
      << "\"model\":\""   << JsonEscape(model)  << "\","
      << "\"host\":\""    << JsonEscape(host)   << "\","
      << "\"path\":\""    << JsonEscape(path)   << "\""
      << "}";
    return j.str();
  });

  w.bind("nativeWriteSchema", [](const std::string& args) -> std::string {
    auto parts = ParseJsonStringArray(args);
    if (parts.size() < 4) return "false";
    std::string yaml = ReadAllUtf8(SchemaFilePath());
    if (yaml.empty()) return "false";
    yaml = ReplaceYamlField(yaml, "api_key", parts[0], true);
    yaml = ReplaceYamlField(yaml, "model",   parts[1], false);
    yaml = ReplaceYamlField(yaml, "host",    parts[2], false);
    yaml = ReplaceYamlField(yaml, "path",    parts[3], false);
    if (!WriteAllUtf8(SchemaFilePath(), yaml)) return "false";
    // Fire restart asynchronously so the UI can close.
    std::thread([]() { RedeployAndRestartServer(); }).detach();
    return "true";
  });

  // Per-provider keyring. Read returns the raw JSON file contents (or "{}"
  // if missing). Write replaces the file with the JSON string supplied by
  // the UI. The UI is responsible for the JSON schema.
  w.bind("nativeReadKeyring", [](const std::string& /*args*/) -> std::string {
    std::string raw = ReadAllUtf8(KeyringFilePath());
    if (raw.empty()) raw = "{}";
    return "\"" + JsonEscape(raw) + "\"";
  });
  w.bind("nativeWriteKeyring", [](const std::string& args) -> std::string {
    auto parts = ParseJsonStringArray(args);
    if (parts.empty()) return "false";
    fs::path p = KeyringFilePath();
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    return WriteAllUtf8(p, parts[0]) ? "true" : "false";
  });

  // Classify a free-form / chip target into A/B/C/D via one LLM call.
  // ASYNC: the HTTP request must NOT block the webview UI thread (a sync
  // bind doing WinHTTP froze the panel on "分类中…" forever). We spawn a
  // worker thread and resolve(seq) when done. Result is a JSON string
  // value: "A"/"B"/"C"/"D" on success, or "error:<msg>".
  w.bind("nativeClassifyLang",
    [&w](const std::string& seq, const std::string& req, void* /*arg*/) {
      std::thread([&w, seq, req]() {
        auto done = [&](const std::string& s) {
          w.resolve(seq, 0, "\"" + JsonEscape(s) + "\"");
        };
        auto parts = ParseJsonStringArray(req);
        if (parts.empty()) { done("error:empty target"); return; }
        std::string target = parts[0];

        std::string yaml = ReadAllUtf8(SchemaFilePath());
        std::string apikey = ExtractYamlField(yaml, "api_key");
        std::string model  = ExtractYamlField(yaml, "model");
        std::string host   = ExtractYamlField(yaml, "host");
        std::string path   = ExtractYamlField(yaml, "path");
        if (model.empty()) model = "deepseek-chat";
        if (host.empty())  host  = "api.deepseek.com";
        if (path.empty())  path  = "/v1/chat/completions";
        if (apikey.empty()) { done("error:API key 未配置"); return; }

        std::string classify = ReadPromptSection("CLASSIFY");
        if (classify.empty()) { done("error:分类 prompt 缺失（typeanything_prompts.txt）"); return; }
        classify = ReplaceAllStr(classify, "{TARGET}", target);

        std::ostringstream payload;
        payload << "{\"model\":\"" << JsonEscape(model) << "\","
                << "\"temperature\":0,"
                << "\"max_tokens\":4,"
                << "\"messages\":["
                << "{\"role\":\"system\",\"content\":\"" << JsonEscape(classify) << "\"},"
                << "{\"role\":\"user\",\"content\":\"" << JsonEscape(target) << "\"}]}";

        std::string err;
        std::string resp = HttpsPost(host, path, apikey, payload.str(), &err);
        if (resp.empty()) { done("error:" + (err.empty() ? "网络失败" : err)); return; }

        std::string content = ExtractContent(resp);
        for (char c : content) {
          if (c == 'A' || c == 'B' || c == 'C' || c == 'D') { done(std::string(1, c)); return; }
        }
        done("error:分类返回无法解析（" + content.substr(0, 20) + "）");
      }).detach();
    }, nullptr);

  w.bind("nativeOpenUrl", [](const std::string& args) -> std::string {
    auto parts = ParseJsonStringArray(args);
    if (parts.empty()) return "false";
    ShellExecuteW(nullptr, L"open", Utf8ToWide(parts[0]).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
    return "true";
  });

  w.bind("nativeClose", [&w](const std::string& /*args*/) -> std::string {
    w.terminate();
    return "true";
  });

  // ─── Window setup + Mica ──────────────────────────────────
  HWND hwnd = (HWND)w.window();
  ApplyMica(hwnd);

  // Single-instance: publish our HWND in a named file mapping so a
  // secondary launch can find us and PostMessage. Install a subclass on
  // the webview's HWND to handle WM_TA_SHOWPAGE.
  HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                    PAGE_READWRITE, 0, sizeof(HWND),
                                    TA_HWND_MAP_NAME);
  if (hMap) {
    HWND* p = (HWND*)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(HWND));
    if (p) { *p = hwnd; UnmapViewOfFile(p); }
    // Intentionally leak hMap — kernel object stays alive until process exit.
  }
  g_webview = &w;
  g_original_wndproc = (WNDPROC)SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, (LONG_PTR)TaSubclassProc);

  // Force the new window to the foreground. When WeaselServer (MEDIUM IL,
  // possibly background) ShellExecutes us, the OS doesn't always grant
  // foreground focus to the new process — model-config launches in
  // particular have been observed to open behind other windows. Use the
  // AttachThreadInput trick to bypass SetForegroundWindow restrictions.
  {
    ShowWindow(hwnd, SW_SHOWNORMAL);
    HWND fg = GetForegroundWindow();
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD my_tid = GetCurrentThreadId();
    if (fg_tid && fg_tid != my_tid) {
      AttachThreadInput(my_tid, fg_tid, TRUE);
      SetForegroundWindow(hwnd);
      BringWindowToTop(hwnd);
      AttachThreadInput(my_tid, fg_tid, FALSE);
    } else {
      SetForegroundWindow(hwnd);
      BringWindowToTop(hwnd);
    }
  }

  // ─── Load the UI HTML ─────────────────────────────────────
  fs::path index = ExeDir() / "ui" / "index.html";
  std::string url = FileUrl(index) + "?page=" + page;
  w.navigate(url);

  w.run();
  return 0;
}
