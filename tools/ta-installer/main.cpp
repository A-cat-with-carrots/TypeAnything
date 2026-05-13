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
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

static std::pair<const void*, DWORD> LoadEmbedded(const wchar_t* name) {
  HRSRC h = FindResourceW(nullptr, name, RT_RCDATA);
  if (!h) return {nullptr, 0};
  DWORD sz = SizeofResource(nullptr, h);
  HGLOBAL g = LoadResource(nullptr, h);
  if (!g) return {nullptr, 0};
  return {LockResource(g), sz};
}

static bool WriteEmbeddedToFile(const wchar_t* res_name, const fs::path& dst) {
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
static bool WriteEmbeddedToLockableFile(const wchar_t* res_name,
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

static fs::path RimeUserDir() {
  PWSTR p = nullptr; std::wstring root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &p))) {
    root = p; CoTaskMemFree(p);
  }
  return fs::path(root) / L"Rime";
}

static fs::path TempUiDir() {
  WCHAR tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return fs::path(tmp) / L"ta-installer-ui";
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

static void StartShown(const fs::path& exe) {
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                     0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }
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
};

static void DoInstall(InstallOptions opts) {
  bool reboot_needed = false;

  // 1. Stop running Weasel-family processes.
  PushStatus(5, "停止运行中的输入法服务 …", "taskkill WeaselServer / WeaselDeployer / ta-settings");
  for (const wchar_t* img : {L"WeaselServer.exe", L"WeaselDeployer.exe",
                              L"WeaselTrayIcon.exe", L"ta-settings.exe"}) {
    KillByName(img);
  }
  Sleep(800);

  // 2. Backup originals once, then write all binaries.
  fs::path wdir = WeaselDir();
  if (!fs::exists(wdir)) {
    std::error_code ec; fs::create_directories(wdir, ec);
  }

  struct Pair { const wchar_t* res; const wchar_t* leaf; int weight; };
  std::vector<Pair> binaries = {
    {L"RIME_DLL",       L"rime.dll",          12},
    {L"WEASELX64_DLL",  L"weaselx64.dll",     12},
    {L"WEASELSERVER",   L"WeaselServer.exe",  10},
    {L"WEASELDEPLOYER", L"WeaselDeployer.exe", 8},
    {L"TA_SETTINGS",    L"ta-settings.exe",    8},
    {L"WV2_LOADER",     L"WebView2Loader.dll", 6},
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
  fs::path sys32_dll = L"C:\\WINDOWS\\system32\\weasel.dll";
  fs::path sys32_bak = sys32_dll.wstring() + L".bak";
  if (fs::exists(sys32_dll) && !fs::exists(sys32_bak)) {
    std::error_code ec; fs::copy_file(sys32_dll, sys32_bak, ec);
  }
  PushStatus(64, "替换 system32\\weasel.dll …", "system32\\weasel.dll");
  WriteEmbeddedToLockableFile(L"WEASELX64_DLL", sys32_dll, &reboot_needed);

  // 4. ta-settings ui directory.
  fs::path ui = wdir / L"ui";
  PushStatus(70, "释放 UI 资源 …", "ta-settings ui/");
  WriteEmbeddedToFile(L"TARGET_UI_HTML", ui / L"index.html");
  WriteEmbeddedToFile(L"TARGET_UI_CSS",  ui / L"style.css");
  WriteEmbeddedToFile(L"TARGET_UI_JS",   ui / L"app.js");
  WriteEmbeddedToFile(L"TARGET_UI_PNG",  ui / L"fish.png");

  // 5. Schema yaml with injected api_key and target_lang.
  PushStatus(78, "写入用户配置 …", "%APPDATA%\\Rime\\typeanything.schema.yaml");
  {
    auto [ptr, sz] = LoadEmbedded(L"SCHEMA_YAML");
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

    // Default custom.yaml so Deployer only sees TypeAnything.
    std::ofstream cf(udir / L"default.custom.yaml", std::ios::binary | std::ios::trunc);
    cf << "patch:\n  schema_list:\n    - schema: typeanything\n";

    // Seed lang.txt with target (so ResolveTargetLang has something on Enter
    // before user opens the settings panel).
    std::ofstream lf(udir / L"typeanything_lang.txt", std::ios::binary | std::ios::trunc);
    lf << (opts.target_lang.empty() ? "English" : opts.target_lang);
  }

  // 6. Patch HKLM TSF profile descriptions.
  PushStatus(84, "注册 TSF 描述 …", "HKLM CTF\\TIP profile description = TypeAnything");
  PatchTsfProfileDescriptions();

  // 7. Drop other schemas from Weasel data dir.
  PushStatus(88, "隐藏其他方案 …", "data/*.schema.yaml");
  {
    fs::path data = wdir / L"data";
    fs::path orig = wdir / L"data.original";
    if (fs::exists(data) && !fs::exists(orig)) {
      std::error_code ec; fs::copy(data, orig, fs::copy_options::recursive, ec);
    }
    if (fs::exists(data)) {
      for (auto& e : fs::directory_iterator(data)) {
        auto name = e.path().filename().wstring();
        if (name.size() > 12 && name.substr(name.size() - 12) == L".schema.yaml") {
          std::error_code ec; fs::remove(e.path(), ec);
        }
        if (name.size() > 10 && name.substr(name.size() - 10) == L".dict.yaml"
            && name.compare(0, 11, L"luna_pinyin") != 0) {
          std::error_code ec; fs::remove(e.path(), ec);
        }
      }
    }
  }

  // 8. Redeploy schema + start server.
  PushStatus(92, "编译方案 …", "WeaselDeployer /deploy");
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
    PushStatus(95, "方案编译耗时偏长，继续启动 server", "deployer poll fallback",
               "done");
  } else {
    PushStatus(95, "方案编译完成", "prism.bin OK", "done");
  }

  // 9. Start WeaselServer so the tray icon comes up.
  PushStatus(98, "启动输入法服务 …", "WeaselServer.exe");
  StartShown(wdir / L"WeaselServer.exe");

  // 10. Register autostart (HKLM\Run already done by upstream MSI usually,
  //     but in case it's missing we add it now).
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

  std::string final_msg = reboot_needed
      ? "部分文件被占用，已排队重启替换。重启电脑后完全生效。"
      : "全部完成。打开新应用即可使用。";
  PushStatus(100, final_msg, final_msg, "done", true);
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

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  // Force a writable WebView2 user data folder (Program Files is read-only).
  wchar_t lad[MAX_PATH] = {0};
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, lad))) {
    std::wstring ud = std::wstring(lad) + L"\\TypeAnything\\WebView2-installer";
    std::error_code ec; fs::create_directories(ud, ec);
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", ud.c_str());
  }

  // Extract installer ui to TEMP so webview can navigate to it.
  fs::path ui_dir = TempUiDir();
  std::error_code ec; fs::create_directories(ui_dir, ec);
  WriteEmbeddedToFile(L"INSTALL_HTML", ui_dir / L"index.html");
  WriteEmbeddedToFile(L"INSTALL_CSS",  ui_dir / L"style.css");
  WriteEmbeddedToFile(L"INSTALL_JS",   ui_dir / L"app.js");
  WriteEmbeddedToFile(L"INSTALL_PNG",  ui_dir / L"fish.png");

  webview::webview w(false, nullptr);
  w.set_title("TypeAnything 安装");
  w.set_size(640, 660, WEBVIEW_HINT_NONE);
  w.set_size(560, 540, WEBVIEW_HINT_MIN);

  std::atomic<bool> installing{false};

  // Bridge: begin install.
  w.bind("nativeBeginInstall", [&](const std::string& args) -> std::string {
    auto p = ParseJsonStringArray(args);
    if (p.size() < 2) return "false";
    if (installing.exchange(true)) return "false";
    InstallOptions opts{p[0], p[1]};
    std::thread([opts]() { DoInstall(opts); }).detach();
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

  w.navigate(FileUrl(ui_dir / L"index.html"));
  w.run();

  stop_pump.store(true);
  pump.join();
  return 0;
}
