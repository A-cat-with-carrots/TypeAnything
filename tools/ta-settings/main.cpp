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
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM (used in NCHITTEST)
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <winhttp.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
static fs::path ClassifyLogPath() { return RimeUserDir() / L"typeanything_classify.log"; }

// ─── Classify log (best-effort, never throws) ────────────────
//
// Format (must stay consistent across ta-settings/processor/installer):
//   [YYYY-MM-DD HH:MM:SS] <KIND> target="<short>"
//     host=<host> path=<path> model=<model>
//     http_status=<status>
//     response[0..500]=<truncated raw body, single line, no key>
//     parsed=<what we extracted>
//     result=<final outcome / error message>
//   ---
//
// Auto-rotate at 1 MB: the current file is moved over .log.1 (any prior
// rotation overwritten) and a fresh log started. We never log the API key.

// Replace CR/LF with spaces so multi-line raw bodies stay on one line.
static std::string LogOneLine(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    if (c == '\n' || c == '\r') r.push_back(' ');
    else                        r.push_back(c);
  }
  return r;
}

// Truncate to `limit` UTF-8 bytes (caller's contract: char-count == byte-count
// is acceptable here; we slice on byte boundary then strip a trailing partial
// UTF-8 lead so we don't write a half-codepoint).
static std::string LogTruncate(const std::string& s, size_t limit,
                               const char* suffix) {
  if (s.size() <= limit) return s;
  size_t cut = limit;
  // Back off if the cut lands mid-multi-byte sequence.
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + suffix;
}

static std::string LogTimestamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

// Append `body` (already fully formatted, including trailing "---\n") to the
// classify log; rotate first if the file is > 1 MB. Serialized via a static
// mutex so concurrent classify calls don't interleave.
static void ClassifyLog(const std::string& body) {
  static std::mutex log_mu;
  std::lock_guard<std::mutex> lk(log_mu);
  fs::path p = ClassifyLogPath();
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  // Rotate at 1 MB.
  uintmax_t sz = 0;
  if (fs::exists(p, ec)) sz = fs::file_size(p, ec);
  if (!ec && sz > 1024 * 1024) {
    fs::path rot = p;
    rot += L".1";
    fs::remove(rot, ec);
    fs::rename(p, rot, ec);
  }
  std::ofstream f(p, std::ios::binary | std::ios::app);
  if (!f.is_open()) return;
  f.write(body.data(), (std::streamsize)body.size());
}

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
// If `status_code` is non-null, it is filled with the HTTP status (0 if the
// request never reached the response stage).
static std::string HttpsPost(const std::string& host, const std::string& path,
                             const std::string& api_key,
                             const std::string& body, std::string* err,
                             int* status_code = nullptr) {
  if (status_code) *status_code = 0;
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
  if (status_code) *status_code = (int)status;
  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  if (!ok) return fail("网络请求失败（检查网络 / 防火墙）");
  if (status == 401) return fail("HTTP 401 — API key 无效或未配置");
  if (status >= 400) return fail("HTTP " + std::to_string(status));
  return resp;
}

// Pull the assistant message content out of an OpenAI-compatible JSON reply.
//
// Handles two response shapes:
//   • OpenAI / DeepSeek / Moonshot / 智谱 / Ollama-v1:
//       "content":"the text"
//   • Anthropic /v1/messages:
//       "content":[{"type":"text","text":"the text"}]
//     — naive `find '"' after ':'` matches the field name "type" and yields
//       the literal string "type" as content (which is what issue #2 hit).
static std::string ExtractContent(const std::string& json) {
  size_t k = json.find("\"content\"");
  if (k == std::string::npos) return "";
  size_t colon = json.find(':', k);
  if (colon == std::string::npos) return "";

  // Walk past whitespace after the colon to see the value's first char.
  size_t v = colon + 1;
  while (v < json.size() && (json[v] == ' ' || json[v] == '\t' ||
                             json[v] == '\n' || json[v] == '\r')) ++v;
  if (v >= json.size()) return "";

  // Anthropic shape: `"content":[{"type":"text","text":"..."}]`.
  // Locate the FIRST `"text":"..."` after the array open and extract that.
  size_t q1;
  if (json[v] == '[') {
    size_t t = json.find("\"text\"", v);
    if (t == std::string::npos) return "";
    size_t tColon = json.find(':', t);
    if (tColon == std::string::npos) return "";
    q1 = json.find('"', tColon + 1);
  } else {
    // OpenAI shape: `"content":"..."`. Skip to opening quote.
    q1 = json.find('"', v);
  }
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

  // Force light caption — our body palette is blue-white regardless of the
  // system theme, so following the OS dark mode produces a black title bar
  // sitting above a white body.
  BOOL dark = FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

  // Hide the icon in the title bar (taskbar icon stays).
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_DLGMODALFRAME);
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
  SendMessageW(hwnd, WM_SETICON, ICON_BIG, 0);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
               SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// ───────────────────────── main ──────────────────────────────

// Subclass state (only one ta-settings window per process; module-level OK).
static WNDPROC g_original_wndproc = nullptr;
static webview::webview* g_webview = nullptr;

// Frameless titlebar geometry (must match .titlebar height in style.css and
// .window-controls width = N * .wc width). Top strip is the drag region;
// right cluster reserves WM_NCHITTEST → HTCLIENT so the custom min/close
// buttons receive their own clicks.
static const int kTitlebarHpx     = 42;
static const int kBtnClusterWpx   = 100;

static LRESULT CALLBACK TaSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  // Frameless: claim the full window as client + custom hit-test the top
  // strip for drag / the cluster for buttons / edges for resize.
  if (msg == WM_NCCALCSIZE) {
    if (wp == TRUE) return 0;
  } else if (msg == WM_NCHITTEST) {
    RECT rc; GetWindowRect(hwnd, &rc);
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    const int RESIZE = 8;
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
    int titlebar_bottom = rc.top + kTitlebarHpx;
    int btn_left_edge   = rc.right - kBtnClusterWpx;
    if (pt.y < titlebar_bottom && pt.x < btn_left_edge) return HTCAPTION;
    return HTCLIENT;
  }
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

// One-shot helper: install the TypeAnything TIP into the *current user's*
// input language list via input.dll!InstallLayoutOrTip. ta-installer runs
// elevated and writes the machine-wide HKLM CTF\TIP registration, but the
// per-user "show in Win+Space" mapping lives in HKCU and must be set as
// the interactive user. The installer therefore calls back into us with
// `--register-ime` via CreateProcessWithTokenW(shell-user-token), and we
// just touch the API + exit.
//
// Layout string format: "<langid>:{TIP-CLSID}{LANG-PROFILE-GUID}".
//   0804 = zh-CN.
//   {A3F4CDED-...} = TypeAnything TIP CLSID (matches installer & WeaselTSF).
//   {3D02CAB6-...} = TypeAnything LanguageProfile GUID.
static int RegisterImeForCurrentUser() {
  HMODULE h = LoadLibraryW(L"input.dll");
  if (!h) return 1;
  typedef BOOL (WINAPI *InstallLayoutOrTipFn)(LPCWSTR, DWORD);
  auto fn = (InstallLayoutOrTipFn)GetProcAddress(h, "InstallLayoutOrTip");
  if (!fn) { FreeLibrary(h); return 1; }
  BOOL ok = fn(
      L"0804:{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}"
      L"{3D02CAB6-2B8E-4781-BA20-1C9267529467}",
      0);
  FreeLibrary(h);
  return ok ? 0 : 1;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  // Headless mode — install IME profile into the current user's HKCU and
  // exit. Triggered by ta-installer near the end of install (issue #13).
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool register_ime = false;
    if (argv) {
      for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--register-ime") == 0) { register_ime = true; break; }
      }
      LocalFree(argv);
    }
    if (register_ime) return RegisterImeForCurrentUser();
  }

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
  //
  // Default size 700x786 is too tall for 1366x768 / 1536x864 @ 125% DPI
  // (issue #10). Clamp to work-area minus a small safety margin so the
  // panel always fits and "保存" button stays reachable. SM_CXFULLSCREEN /
  // SM_CYFULLSCREEN report the maximized client area (work area minus
  // taskbar), which is what we want here.
  {
    int sw = GetSystemMetrics(SM_CXFULLSCREEN);
    int sh = GetSystemMetrics(SM_CYFULLSCREEN);
    int want_w = 700, want_h = 786;
    if (sw > 0 && want_w > sw - 80) want_w = sw - 80;
    if (sh > 0 && want_h > sh - 80) want_h = sh - 80;
    if (want_w < 480) want_w = 480;
    if (want_h < 520) want_h = 520;
    w.set_size(want_w, want_h, WEBVIEW_HINT_NONE);
  }
  w.set_size(480, 520, WEBVIEW_HINT_MIN);

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
        // Shared log-record state. We append exactly ONE entry per
        // classify call (success OR failure) right before resolving.
        struct LogRec {
          std::string target;
          std::string host;
          std::string path;
          std::string model;
          int         status = 0;
          std::string response;
          std::string parsed;
          std::string result;
          std::string kind = "CLASSIFY";
        } rec;

        auto write_log = [&]() {
          std::ostringstream o;
          o << "[" << LogTimestamp() << "] " << rec.kind
            << " target=\"" << LogTruncate(LogOneLine(rec.target), 50, "...") << "\"\n"
            << "  host=" << rec.host << " path=" << rec.path
            << " model=" << rec.model << "\n"
            << "  http_status=" << rec.status << "\n"
            << "  response[0..500]="
            << LogTruncate(LogOneLine(rec.response), 500, "...(truncated)") << "\n"
            << "  parsed=" << LogOneLine(rec.parsed) << "\n"
            << "  result=" << LogOneLine(rec.result) << "\n"
            << "---\n";
          ClassifyLog(o.str());
        };

        auto done = [&](const std::string& s) {
          rec.result = s;
          write_log();
          w.resolve(seq, 0, "\"" + JsonEscape(s) + "\"");
        };

        auto parts = ParseJsonStringArray(req);
        if (parts.empty()) { done("error:empty target"); return; }
        std::string target = parts[0];
        rec.target = target;

        std::string yaml = ReadAllUtf8(SchemaFilePath());
        std::string apikey = ExtractYamlField(yaml, "api_key");
        std::string model  = ExtractYamlField(yaml, "model");
        std::string host   = ExtractYamlField(yaml, "host");
        std::string path   = ExtractYamlField(yaml, "path");
        if (model.empty()) model = "deepseek-chat";
        if (host.empty())  host  = "api.deepseek.com";
        if (path.empty())  path  = "/v1/chat/completions";
        rec.host = host; rec.path = path; rec.model = model;
        if (apikey.empty()) { done("error:API key 未配置"); return; }

        std::string classify = ReadPromptSection("CLASSIFY");
        if (classify.empty()) { done("error:分类 prompt 缺失（typeanything_prompts.txt）"); return; }
        classify = ReplaceAllStr(classify, "{TARGET}", target);

        std::ostringstream payload;
        payload << "{\"model\":\"" << JsonEscape(model) << "\","
                << "\"temperature\":0,"
                << "\"max_tokens\":16,"
                << "\"messages\":["
                << "{\"role\":\"system\",\"content\":\"" << JsonEscape(classify) << "\"},"
                << "{\"role\":\"user\",\"content\":\"" << JsonEscape(target) << "\"}]}";

        std::string err;
        int http_status = 0;
        std::string resp = HttpsPost(host, path, apikey, payload.str(), &err,
                                     &http_status);
        rec.status = http_status;
        rec.response = resp;
        if (resp.empty()) {
          rec.parsed = "";
          done("error:" + (err.empty() ? "网络失败" : err));
          return;
        }

        // Scan from the END for the last A/B/C/D. Reasons:
        //  - LLM tends to put its final answer last (e.g. "Type: A", "Category: B").
        //  - Word-leading capitals (e.g. "Answer", "Category") used to false-match.
        std::string content = ExtractContent(resp);
        rec.parsed = content;
        for (auto it = content.rbegin(); it != content.rend(); ++it) {
          char c = *it;
          if (c == 'A' || c == 'B' || c == 'C' || c == 'D') { done(std::string(1, c)); return; }
        }
        done("error:分类返回无法解析（" + content.substr(0, 20) + "）");
      }).detach();
    }, nullptr);

  // Open the classify log file in the OS default viewer (notepad).
  w.bind("nativeOpenClassifyLog", [](const std::string& /*args*/) -> std::string {
    fs::path p = ClassifyLogPath();
    // Create an empty file if missing so notepad doesn't prompt the user.
    if (!fs::exists(p)) {
      std::error_code ec;
      fs::create_directories(p.parent_path(), ec);
      std::ofstream f(p, std::ios::binary | std::ios::app);
    }
    ShellExecuteW(nullptr, L"open", p.wstring().c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
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

  // ─── Window setup + Mica + frameless ──────────────────────
  HWND hwnd = (HWND)w.window();
  w.bind("nativeMinimize", [hwnd](const std::string&) -> std::string {
    ShowWindow(hwnd, SW_MINIMIZE);
    return "true";
  });
  // WebView2's child HWND covers the entire client area and intercepts
  // every mouse message before WM_NCHITTEST reaches our parent, so the
  // subclass's HTCAPTION return never runs. Workaround: HTML titlebar
  // mousedown handler invokes this binding, which kicks off the OS's
  // native window drag from the current cursor position.
  w.bind("nativeStartDrag", [hwnd](const std::string&) -> std::string {
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    return "true";
  });
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

  // Frameless: strip OS title bar + system menu so the HTML titlebar owns
  // the top edge. WS_THICKFRAME stays so the DWM shadow + Aero snap +
  // resize border still work. TaSubclassProc handles WM_NCCALCSIZE and
  // WM_NCHITTEST to make the client area cover the full window and
  // route the top strip to HTCAPTION drag.
  {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_SYSMENU);
    style |= WS_THICKFRAME | WS_MINIMIZEBOX;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    MARGINS m{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }

  // Clamp the OUTER window rect to the work area (issue #10).
  //
  // set_size() above sizes the client area, but our non-client frame
  // (WS_THICKFRAME borders + the 1px DWM top frame) inflates the outer
  // rect by ~14x39 px, so the panel can still overshoot a small-screen
  // work area even after the SM_CXFULLSCREEN-based clamp. Measure the
  // ACTUAL outer rect now and re-clamp + reposition.
  //
  // Reference: hrdAI3's #10 reopen — on 1536x864 @ 125% DPI, work area
  // was 1536x816 and the outer rect came out 714x823, 84 px below the
  // bottom. SystemParametersInfo(SPI_GETWORKAREA, ...) is the correct
  // source (it subtracts the taskbar); SM_CYFULLSCREEN returned the
  // wrong number here because it reports the maximized CLIENT area for
  // a window with a regular caption, not a frameless WS_THICKFRAME one.
  {
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) && wa.right > wa.left
        && wa.bottom > wa.top) {
      const int MARGIN = 24;
      int max_w = (wa.right - wa.left) - MARGIN;
      int max_h = (wa.bottom - wa.top) - MARGIN;
      RECT cur{};
      GetWindowRect(hwnd, &cur);
      int outer_w = cur.right - cur.left;
      int outer_h = cur.bottom - cur.top;
      if (outer_w > max_w) outer_w = max_w;
      if (outer_h > max_h) outer_h = max_h;
      if (outer_w < 480) outer_w = 480;
      if (outer_h < 520) outer_h = 520;
      int x = cur.left, y = cur.top;
      if (x < wa.left) x = wa.left;
      if (y < wa.top)  y = wa.top;
      if (x + outer_w > wa.right)  x = wa.right  - outer_w;
      if (y + outer_h > wa.bottom) y = wa.bottom - outer_h;
      SetWindowPos(hwnd, nullptr, x, y, outer_w, outer_h,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

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
