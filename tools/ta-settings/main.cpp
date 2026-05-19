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
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

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
  std::wstring cmd = L"taskkill /F /IM ";
  cmd += image;
  cmd += L" /T";
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
  if (page == "model") {
    // 4 form fields + preset row + intro + get-key link + bottom buttons.
    // Tested heights: 720 fits all content without scroll on a 1080p
    // display with 100% scaling.
    // Trimmed: was 620, dropped two rows after removing the get-key link
    // and the install-prompt subtitle in the model form.
    w.set_size(660, 680, WEBVIEW_HINT_NONE);
    w.set_size(560, 540, WEBVIEW_HINT_MIN);
  } else {
    // Language picker is intentionally scrollable; pick a comfortable
    // initial height that shows 2-3 chip categories.
    w.set_size(700, 800, WEBVIEW_HINT_NONE);
    w.set_size(560, 580, WEBVIEW_HINT_MIN);
  }

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

  // ─── Load the UI HTML ─────────────────────────────────────
  fs::path index = ExeDir() / "ui" / "index.html";
  std::string url = FileUrl(index) + "?page=" + page;
  w.navigate(url);

  w.run();
  return 0;
}
