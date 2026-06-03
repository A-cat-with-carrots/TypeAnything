// TypeAnything processor implementation — async, no placeholder, multi-lang.
//
// Flow:
//   * commit_notifier hook accumulates Chinese commits.
//   * On Enter (no active composition, accumulated non-empty):
//       1. spawn worker thread to call LLM (Chinese stays visible in app)
//       2. when LLM returns, delete the original Chinese chars and paste
//          translation via clipboard + Ctrl+V
//       3. version-check: if user typed something else after step 1, drop result

#include "typeanything_processor.h"

#include <rime/composition.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/schema.h>
#include <rime/config.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <mutex>
#include <ctime>
#include <filesystem>

namespace typeanything {

// Languages supported. Tray "Switch Language" submenu writes the chosen code
// to %APPDATA%\Rime\typeanything_lang.txt; plugin reads it on each Enter.
struct LangEntry {
  const char* code;
  const char* full_name;   // used in LLM prompt
};

static const LangEntry kLangs[] = {
  {"en", "English"},
  {"ja", "Japanese"},
  {"ko", "Korean"},
  {"yue","Cantonese"},
  {"fr", "French"},
  {"de", "German"},
  {"es", "Spanish"},
  {"it", "Italian"},
  {"pt", "Portuguese"},
  {"ru", "Russian"},
  {"ar", "Arabic"},
  {"vi", "Vietnamese"},
  {"th", "Thai"},
  {"id", "Indonesian"},
  {"tr", "Turkish"},
  {"hi", "Hindi"},
  {"nl", "Dutch"},
  {"pl", "Polish"},
  {"sv", "Swedish"},
  {"el", "Greek"},
  {"he", "Hebrew"},
  {"fa", "Persian"},
  {"uk", "Ukrainian"},
  {"cs", "Czech"},
  {"da", "Danish"},
  {"fi", "Finnish"},
  {"no", "Norwegian"},
  {"hu", "Hungarian"},
  {"ro", "Romanian"},
  {"ms", "Malay"},
};

namespace {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string JsonEscape(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '"') r.push_back('\\');
    if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
      r.append(buf);
      continue;
    }
    r.push_back(c);
  }
  return r;
}

// Handles two response shapes:
//   OpenAI / DeepSeek / Moonshot / 智谱 / Ollama-v1: "content":"..."
//   Anthropic /v1/messages:                        "content":[{"type":"text","text":"..."}]
// Without the Anthropic branch the naive scan grabs the field name "type"
// as the value (see issue #2).
std::string ExtractContent(const std::string& json) {
  std::string needle = "\"content\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";

  size_t v = pos + 1;
  while (v < json.size() && (json[v] == ' ' || json[v] == '\t' ||
                             json[v] == '\n' || json[v] == '\r')) ++v;
  if (v >= json.size()) return "";

  if (json[v] == '[') {
    size_t t = json.find("\"text\"", v);
    if (t == std::string::npos) return "";
    pos = json.find(':', t);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
  } else {
    pos = json.find('"', v);
  }
  if (pos == std::string::npos) return "";
  ++pos;
  std::string out;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '\\' && pos < json.size()) {
      char esc = json[pos++];
      switch (esc) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'u': {
          if (pos + 4 <= json.size()) {
            unsigned cp = 0;
            for (int i = 0; i < 4; ++i) {
              char h = json[pos + i];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
            }
            pos += 4;
            if (cp < 0x80) {
              out.push_back((char)cp);
            } else if (cp < 0x800) {
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
        default: out.push_back(esc); break;
      }
    } else if (c == '"') {
      return out;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string WinHttpPost(const std::wstring& host,
                        const std::wstring& path,
                        const std::string& bearer_token,
                        const std::string& body,
                        DWORD timeout_ms = 15000,
                        int* status_code = nullptr) {
  if (status_code) *status_code = 0;
  HINTERNET h_session = WinHttpOpen(L"TypeAnything/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h_session) return "";
  WinHttpSetTimeouts(h_session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

  HINTERNET h_conn = WinHttpConnect(h_session, host.c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!h_conn) { WinHttpCloseHandle(h_session); return ""; }

  HINTERNET h_req = WinHttpOpenRequest(h_conn, L"POST", path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
  if (!h_req) {
    WinHttpCloseHandle(h_conn);
    WinHttpCloseHandle(h_session);
    return "";
  }

  std::wstring auth = L"Authorization: Bearer " + Utf8ToWide(bearer_token);
  WinHttpAddRequestHeaders(h_req, auth.c_str(), (DWORD)-1L,
                           WINHTTP_ADDREQ_FLAG_ADD);
  WinHttpAddRequestHeaders(h_req,
                           L"Content-Type: application/json",
                           (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

  BOOL sent = WinHttpSendRequest(h_req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
  std::string result;
  if (sent && WinHttpReceiveResponse(h_req, nullptr)) {
    if (status_code) {
      DWORD st = 0, slen = sizeof(st);
      WinHttpQueryHeaders(h_req,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &st, &slen,
                          WINHTTP_NO_HEADER_INDEX);
      *status_code = (int)st;
    }
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(h_req, &avail) && avail > 0) {
      std::vector<char> buf(avail);
      DWORD read = 0;
      if (!WinHttpReadData(h_req, buf.data(), avail, &read)) break;
      result.append(buf.data(), read);
    }
  }
  WinHttpCloseHandle(h_req);
  WinHttpCloseHandle(h_conn);
  WinHttpCloseHandle(h_session);
  return result;
}


// ─── Translate log (best-effort, fire-and-forget) ────────────
//
// Writes %APPDATA%\Rime\typeanything_translate.log. Same format as the
// classify log (see ta-settings/main.cpp). Critical: this runs in the IME
// hot path — we MUST NOT block typing on disk I/O. The caller bundles all
// fields into TranslateLogRec and dispatches a detached std::thread that
// performs the write.

struct TranslateLogRec {
  std::string target_lang;       // resolved target language / freeform desc
  char        category = 0;      // 'A'/'B'/'C'/'D' or 0 for legacy/fallback
  std::string input_text;        // raw Chinese input (will be truncated)
  std::string host;
  std::string path;
  std::string model;
  int         http_status = 0;
  std::string response;          // raw response body (will be truncated)
  std::string output;            // extracted translation
  std::string result;            // final outcome ("ok" / error message)
  bool        send_input_ok = false;  // SendInput + clipboard succeeded
};

inline std::string TLogOneLine(const std::string& s) {
  std::string r; r.reserve(s.size());
  for (char c : s) { r.push_back((c == '\n' || c == '\r') ? ' ' : c); }
  return r;
}

inline std::string TLogTruncate(const std::string& s, size_t limit,
                                const char* suffix) {
  if (s.size() <= limit) return s;
  size_t cut = limit;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + suffix;
}

inline std::string TLogTimestamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

inline std::filesystem::path TranslateLogPath() {
  wchar_t appdata[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
    return std::filesystem::path();
  return std::filesystem::path(appdata) / L"Rime" / L"typeanything_translate.log";
}

// Append + rotate at 1 MB. Serialized via a static mutex. Best-effort: any
// error (no AppData, file locked, etc.) is swallowed so the IME never trips.
inline void TranslateLogWrite(const std::string& body) {
  static std::mutex log_mu;
  std::lock_guard<std::mutex> lk(log_mu);
  auto p = TranslateLogPath();
  if (p.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  uintmax_t sz = 0;
  if (std::filesystem::exists(p, ec)) sz = std::filesystem::file_size(p, ec);
  if (!ec && sz > 1024 * 1024) {
    std::filesystem::path rot = p; rot += L".1";
    std::filesystem::remove(rot, ec);
    std::filesystem::rename(p, rot, ec);
  }
  std::ofstream f(p, std::ios::binary | std::ios::app);
  if (!f.is_open()) return;
  f.write(body.data(), (std::streamsize)body.size());
}

// Detached, fire-and-forget. Caller MUST NOT depend on completion.
inline void TranslateLog(TranslateLogRec rec) {
  std::thread([rec = std::move(rec)]() {
    std::ostringstream o;
    o << "[" << TLogTimestamp() << "] TRANSLATE target=\""
      << TLogTruncate(TLogOneLine(rec.target_lang), 50, "...") << "\"\n"
      << "  host=" << rec.host << " path=" << rec.path
      << " model=" << rec.model << "\n"
      << "  category=" << (rec.category ? std::string(1, rec.category) : std::string("-"))
      << " input[0..50]=\""
      << TLogTruncate(TLogOneLine(rec.input_text), 50, "...") << "\"\n"
      << "  http_status=" << rec.http_status << "\n"
      << "  response[0..500]="
      << TLogTruncate(TLogOneLine(rec.response), 500, "...(truncated)") << "\n"
      << "  parsed=" << TLogOneLine(rec.output) << "\n"
      << "  send_input=" << (rec.send_input_ok ? "ok" : "fail") << "\n"
      << "  result=" << TLogOneLine(rec.result) << "\n"
      << "---\n";
    TranslateLogWrite(o.str());
  }).detach();
}

size_t Utf8CodePointCount(const std::string& s) {
  size_t count = 0;
  for (size_t i = 0; i < s.size(); ) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    ++count;
  }
  return count;
}

void SendBackspaces(int count) {
  if (count <= 0) return;
  std::vector<INPUT> inputs(count * 2);
  for (int i = 0; i < count; ++i) {
    inputs[i * 2].type = INPUT_KEYBOARD;
    inputs[i * 2].ki.wVk = VK_BACK;
    inputs[i * 2 + 1].type = INPUT_KEYBOARD;
    inputs[i * 2 + 1].ki.wVk = VK_BACK;
    inputs[i * 2 + 1].ki.dwFlags = KEYEVENTF_KEYUP;
  }
  SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

bool SetClipboardUtf8(const std::string& utf8) {
  std::wstring w = Utf8ToWide(utf8);
  // Retry — clipboard may briefly be locked.
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (OpenClipboard(nullptr)) {
      EmptyClipboard();
      size_t bytes = (w.size() + 1) * sizeof(wchar_t);
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (h) {
        void* p = GlobalLock(h);
        if (p) {
          memcpy(p, w.c_str(), bytes);
          GlobalUnlock(h);
          // On success the system owns h; only free it if we keep ownership.
          if (!SetClipboardData(CF_UNICODETEXT, h)) {
            GlobalFree(h);
          }
        } else {
          GlobalFree(h);  // GlobalLock failed — h is still ours
        }
      }
      CloseClipboard();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void SendPaste() {
  INPUT in[4] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wVk = VK_CONTROL;
  in[1].type = INPUT_KEYBOARD;
  in[1].ki.wVk = 'V';
  in[2].type = INPUT_KEYBOARD;
  in[2].ki.wVk = 'V';
  in[2].ki.dwFlags = KEYEVENTF_KEYUP;
  in[3].type = INPUT_KEYBOARD;
  in[3].ki.wVk = VK_CONTROL;
  in[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, in, sizeof(INPUT));
}

}  // namespace

TypeAnythingProcessor::TypeAnythingProcessor(const rime::Ticket& ticket)
    : rime::Processor(ticket) {
  LoadConfig();
  if (engine_) {
    auto* ctx = engine_->context();
    if (ctx) {
      commit_conn_ = ctx->commit_notifier().connect(
          [this](rime::Context* c) { OnCommit(c); });
    }
  }
}

TypeAnythingProcessor::~TypeAnythingProcessor() {
  if (commit_conn_.connected()) {
    commit_conn_.disconnect();
  }
}

void TypeAnythingProcessor::LoadConfig() {
  endpoint_ = "api.deepseek.com";
  endpoint_path_ = "/v1/chat/completions";
  model_ = "deepseek-chat";
  default_target_lang_ = "English";
  temperature_ = 0.3;

  if (!engine_) return;
  rime::Config* config = engine_->schema()->config();
  if (!config) return;

  std::string s;
  if (config->GetString("typeanything/api_key", &s)) api_key_ = s;
  if (config->GetString("typeanything/model", &s)) model_ = s;
  if (config->GetString("typeanything/host", &s)) endpoint_ = s;
  if (config->GetString("typeanything/path", &s)) endpoint_path_ = s;
  if (config->GetString("typeanything/target_lang", &s)) default_target_lang_ = s;
  double d;
  if (config->GetDouble("typeanything/temperature", &d)) temperature_ = d;
}

// Replace every occurrence of `needle` in `s` with `with`.
static std::string ReplaceAll(std::string s, const std::string& needle,
                              const std::string& with) {
  if (needle.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(needle, pos)) != std::string::npos) {
    s.replace(pos, needle.size(), with);
    pos += with.size();
  }
  return s;
}

// Read %APPDATA%\Rime\typeanything_prompts.txt and return the body of the
// ===<category>=== section (category = 'A'/'B'/'C'/'D'/'CLASSIFY' — but we
// only fetch single-letter sections here). Empty string if file or section
// missing. The file is UTF-8; we never embed Chinese in this .cc (cp936-safe).
static std::string LoadPromptSection(const std::string& section) {
  wchar_t appdata[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
    return "";
  std::wstring path =
      std::wstring(appdata) + L"\\Rime\\typeanything_prompts.txt";
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return "";
  std::string raw;
  char buf[8192];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0)
    raw.append(buf, got);
  CloseHandle(h);

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

std::string TypeAnythingProcessor::ResolveTargetLang() const {
  // Read %APPDATA%\Rime\typeanything_lang.txt — first non-empty line is
  // the active language code. Map to full name. Fall back to schema default.
  wchar_t appdata[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
    return default_target_lang_;
  }
  std::wstring path = std::wstring(appdata) +
                      L"\\Rime\\typeanything_lang.txt";
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return default_target_lang_;
  // Up to 1024 bytes UTF-8 for free-form natural-language descriptions
  // ("中二风格的日语" / "Klingon battle prose" / "学术英语" / ...).
  char buf[1024] = {0};
  DWORD got = 0;
  ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
  CloseHandle(h);
  std::string raw(buf, got);
  // First non-empty, non-comment line wins.
  std::string code;
  std::string line;
  for (size_t i = 0; i <= raw.size(); ++i) {
    char c = (i < raw.size()) ? raw[i] : '\n';
    if (c == '\n' || c == '\r') {
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }
      size_t lead = 0;
      while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) {
        ++lead;
      }
      std::string trimmed = line.substr(lead);
      if (!trimmed.empty() && trimmed[0] != '#') {
        code = trimmed;
        break;
      }
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (code.empty()) return default_target_lang_;
  for (const auto& l : kLangs) {
    if (code == l.code) return l.full_name;
  }
  // Free-form: pass through as the LLM's target description.
  return code;
}

void TypeAnythingProcessor::OnCommit(rime::Context* ctx) {
  if (suppress_capture_.load() || !ctx) return;
  std::string text = ctx->GetCommitText();
  if (text.empty()) return;
  accumulated_ += text;
}

rime::ProcessResult TypeAnythingProcessor::ProcessKeyEvent(
    const rime::KeyEvent& key_event) {
  if (key_event.release()) return rime::kNoop;

  // BackSpace outside composition: user deleting committed Chinese.
  // Pop the last UTF-8 character from accumulated_ so we don't translate
  // text that has already been erased from the document.
  if (key_event.keycode() == XK_BackSpace) {
    rime::Context* ctx = engine_->context();
    if (ctx && !ctx->IsComposing() && !accumulated_.empty()) {
      while (!accumulated_.empty()) {
        unsigned char b = (unsigned char)accumulated_.back();
        accumulated_.pop_back();
        if ((b & 0xC0) != 0x80) break;  // start of a UTF-8 sequence
      }
    }
    return rime::kNoop;  // let BackSpace propagate to the document normally
  }

  // Escape clears the pending translation buffer entirely.
  if (key_event.keycode() == XK_Escape && !accumulated_.empty()) {
    rime::Context* ctx = engine_->context();
    if (ctx && !ctx->IsComposing()) {
      accumulated_.clear();
      return rime::kNoop;
    }
  }

  if (key_event.keycode() != XK_Return) return rime::kNoop;
  if (api_key_.empty()) return rime::kNoop;
  if (accumulated_.empty()) return rime::kNoop;

  rime::Context* ctx = engine_->context();
  if (ctx && ctx->IsComposing()) return rime::kNoop;

  // Pure mode (user toggled "纯净模式" in lang picker): drop the
  // accumulated buffer but return kNoop so Enter falls through to the
  // host app — gives normal newline/submit behavior.
  {
    std::string lang_now = ResolveTargetLang();
    if (lang_now == "off" || lang_now == "Off" || lang_now == "none" ||
        lang_now == "None" || lang_now == "no-translate") {
      accumulated_.clear();
      return rime::kNoop;
    }
  }

  std::string chinese = std::move(accumulated_);
  accumulated_.clear();
  DispatchTranslate(chinese);
  return rime::kAccepted;
}

void TypeAnythingProcessor::DispatchTranslate(const std::string& chinese) {
  uint64_t this_id = request_id_.fetch_add(1) + 1;
  size_t chinese_chars = Utf8CodePointCount(chinese);
  std::string raw_lang = ResolveTargetLang();
  // No-translate mode: skip LLM call when the lang picker selects "off".
  if (raw_lang == "none" || raw_lang == "None" || raw_lang == "no-translate" ||
      raw_lang == "off" || raw_lang == "Off") {
    return;
  }

  // Parse "X:name" category prefix written by ta-settings (X = A/B/C/D).
  // Legacy lang.txt without prefix → category 0 → generic fallback prompt.
  char category = 0;
  std::string lang = raw_lang;
  if (raw_lang.size() >= 2 && raw_lang[1] == ':' &&
      (raw_lang[0] == 'A' || raw_lang[0] == 'B' ||
       raw_lang[0] == 'C' || raw_lang[0] == 'D')) {
    category = raw_lang[0];
    lang = raw_lang.substr(2);
  }

  // Build the category-specific system prompt from the runtime prompts file.
  // Kept out of this .cc to stay cp936-safe and to allow prompt iteration
  // without rebuilding librime.
  std::string system_prompt;
  if (category) {
    std::string tmpl = LoadPromptSection(std::string(1, category));
    if (!tmpl.empty()) system_prompt = ReplaceAll(tmpl, "{LANG}", lang);
  }

  std::string api_key = api_key_;
  std::string model = model_;
  std::string endpoint = endpoint_;
  std::string path = endpoint_path_;
  double temp = temperature_;

  // No placeholder — keep Chinese visible until translation arrives.
  suppress_capture_.store(true);

  // Spawn worker thread for LLM call. When it returns, delete the original
  // Chinese chars and paste the translation.
  std::thread([this, this_id, chinese, chinese_chars, lang, system_prompt,
               api_key, model, endpoint, path, temp, category]() {
    // Logging bundle — populated as we progress; dispatched on every exit
    // path via TranslateLog() (which is itself a detached thread, so no
    // disk I/O blocks the IME hot path).
    TranslateLogRec log_rec;
    log_rec.target_lang = lang;
    log_rec.category    = category;
    log_rec.input_text  = chinese;
    log_rec.host        = endpoint;
    log_rec.path        = path;
    log_rec.model       = model;

    // Fallback (legacy lang.txt with no category prefix, or prompts file
    // missing): plain professional-translator prompt, English-only literals.
    std::string sys = system_prompt;
    if (sys.empty()) {
      sys =
        "You are a professional translator. Translate the user's Chinese into "
        "fluent, idiomatic " + lang + ". Rules:\n"
        "1. Preserve tone (casual, formal, technical, polite, etc.).\n"
        "2. Adapt cultural references and idioms to the target language; do not "
        "literally word-translate.\n"
        "3. Match register: '\xe4\xbd\xa0\xe5\xa5\xbd' in casual chat = 'Hi' "
        "not 'Greetings'.\n"
        "4. Keep proper nouns / English terms / numbers / URLs unchanged.\n"
        "5. Output ONLY the translation. No quotes, no markdown, no notes, no "
        "prefix like 'Translation:'. Just the target text, nothing else.";
    }

    std::ostringstream payload;
    payload << "{"
            << "\"model\":\"" << JsonEscape(model) << "\","
            << "\"temperature\":" << temp << ","
            << "\"messages\":["
            << "{\"role\":\"system\",\"content\":\""
            << JsonEscape(sys) << "\"},"
            << "{\"role\":\"user\",\"content\":\""
            << JsonEscape(chinese) << "\"}"
            << "]}";

    int http_status = 0;
    std::string body = WinHttpPost(Utf8ToWide(endpoint),
                                   Utf8ToWide(path),
                                   api_key, payload.str(), 15000,
                                   &http_status);
    log_rec.http_status = http_status;
    log_rec.response    = body;

    // If user dispatched another translation since we started, abort.
    if (this_id != request_id_.load()) {
      suppress_capture_.store(false);
      log_rec.result = "superseded";
      TranslateLog(std::move(log_rec));
      return;
    }

    std::string english;
    if (!body.empty()) english = ExtractContent(body);
    while (!english.empty() &&
           (english.back() == ' ' || english.back() == '\n' ||
            english.back() == '\r' || english.back() == '\t')) {
      english.pop_back();
    }
    log_rec.output = english;
    if (english.empty()) {
      // Network error / API rejection / empty body. Leave the Chinese in place
      // so user knows translation didn't run; log for diagnostics.
      LOG(ERROR) << "TypeAnything: LLM returned empty for input: " << chinese
                 << "; raw body bytes: " << body.size();
      suppress_capture_.store(false);
      log_rec.result = "empty-response";
      TranslateLog(std::move(log_rec));
      return;
    }

    bool send_ok = false;
    if (SetClipboardUtf8(english)) {
      SendBackspaces((int)chinese_chars);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      SendPaste();
      send_ok = true;
    }
    log_rec.send_input_ok = send_ok;
    log_rec.result = send_ok ? "ok" : "clipboard-fail";
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    suppress_capture_.store(false);
    TranslateLog(std::move(log_rec));
  }).detach();
}

}  // namespace typeanything
