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

std::string ExtractContent(const std::string& json) {
  std::string needle = "\"content\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";
  pos = json.find('"', pos);
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
                        DWORD timeout_ms = 15000) {
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
          SetClipboardData(CF_UNICODETEXT, h);
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

  std::string chinese = std::move(accumulated_);
  accumulated_.clear();
  DispatchTranslate(chinese);
  return rime::kAccepted;
}

void TypeAnythingProcessor::DispatchTranslate(const std::string& chinese) {
  uint64_t this_id = request_id_.fetch_add(1) + 1;
  size_t chinese_chars = Utf8CodePointCount(chinese);
  std::string lang = ResolveTargetLang();
  std::string api_key = api_key_;
  std::string model = model_;
  std::string endpoint = endpoint_;
  std::string path = endpoint_path_;
  double temp = temperature_;

  // No placeholder — keep Chinese visible until translation arrives.
  suppress_capture_.store(true);

  // Spawn worker thread for LLM call. When it returns, delete the original
  // Chinese chars and paste the translation.
  std::thread([this, this_id, chinese, chinese_chars, lang, api_key, model,
               endpoint, path, temp]() {
    // High-quality translation prompt.
    std::string system_prompt =
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

    std::ostringstream payload;
    payload << "{"
            << "\"model\":\"" << JsonEscape(model) << "\","
            << "\"temperature\":" << temp << ","
            << "\"messages\":["
            << "{\"role\":\"system\",\"content\":\""
            << JsonEscape(system_prompt) << "\"},"
            << "{\"role\":\"user\",\"content\":\""
            << JsonEscape(chinese) << "\"}"
            << "]}";

    std::string body = WinHttpPost(Utf8ToWide(endpoint),
                                   Utf8ToWide(path),
                                   api_key, payload.str());

    // If user dispatched another translation since we started, abort.
    if (this_id != request_id_.load()) {
      suppress_capture_.store(false);
      return;
    }

    std::string english;
    if (!body.empty()) english = ExtractContent(body);
    while (!english.empty() &&
           (english.back() == ' ' || english.back() == '\n' ||
            english.back() == '\r' || english.back() == '\t')) {
      english.pop_back();
    }
    if (english.empty()) {
      // Network error / API rejection / empty body. Leave the Chinese in place
      // so user knows translation didn't run; log for diagnostics.
      LOG(ERROR) << "TypeAnything: LLM returned empty for input: " << chinese
                 << "; raw body bytes: " << body.size();
      suppress_capture_.store(false);
      return;
    }

    if (SetClipboardUtf8(english)) {
      SendBackspaces((int)chinese_chars);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      SendPaste();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    suppress_capture_.store(false);
  }).detach();
}

}  // namespace typeanything
