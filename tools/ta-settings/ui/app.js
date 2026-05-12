// TypeAnything settings UI — front-end logic + native bridge
//
// Native bridge functions (exposed by C++ via webview.bind):
//   nativeReadSchema()                      → JSON {api_key, model, host, path}
//   nativeWriteSchema(api_key, model, host, path)  → bool
//   nativeReadLang()                        → string
//   nativeWriteLang(text)                   → bool
//   nativeOpenUrl(url)                      → void
//   nativeClose()                           → void
//
// All bind() calls return Promises.

const PROVIDER_PRESETS = {
  deepseek: { model: "deepseek-chat",    host: "api.deepseek.com",  path: "/v1/chat/completions" },
  openai:   { model: "gpt-4o",           host: "api.openai.com",    path: "/v1/chat/completions" },
  moonshot: { model: "moonshot-v1-8k",   host: "api.moonshot.cn",   path: "/v1/chat/completions" },
  zhipu:    { model: "glm-4-flash",      host: "open.bigmodel.cn",  path: "/api/paas/v4/chat/completions" },
  ollama:   { model: "qwen2.5:7b",       host: "localhost:11434",   path: "/v1/chat/completions" },
};

// Read URL hash / query for which page to show: ?page=lang | ?page=model
function activePage() {
  const url = new URL(window.location.href);
  return url.searchParams.get("page") || "lang";
}

function showPage(name) {
  document.querySelectorAll(".page").forEach(el => el.hidden = true);
  const el = document.getElementById("page-" + name);
  if (el) el.hidden = false;
  document.getElementById("pageTitle").textContent =
    name === "model" ? "模型配置" : "切换语言";
}

function toast(msg, kind = "ok") {
  const el = document.getElementById("toast");
  el.textContent = msg;
  el.className = "toast " + kind;
  el.hidden = false;
  requestAnimationFrame(() => el.classList.add("show"));
  setTimeout(() => {
    el.classList.remove("show");
    setTimeout(() => { el.hidden = true; }, 250);
  }, 1800);
}

// ─── Language page ────────────────────────────────────────────────
async function initLangPage() {
  const input = document.getElementById("langInput");
  const saveBtn = document.getElementById("langSave");

  // Pre-fill from current lang.txt
  try {
    const cur = await window.nativeReadLang();
    if (cur) input.value = cur;
  } catch (e) {}

  // Click chip → fill input
  document.querySelectorAll(".chip").forEach(c => {
    c.addEventListener("click", () => {
      input.value = c.textContent;
      input.focus();
      input.select();
      // brief highlight
      document.querySelectorAll(".chip").forEach(x => x.classList.remove("active"));
      c.classList.add("active");
    });
  });

  // Save: write lang.txt + close
  async function save() {
    const v = input.value.trim();
    if (!v) { toast("目标不能为空", "error"); return; }
    saveBtn.disabled = true;
    try {
      await window.nativeWriteLang(v);
      toast("已保存：" + v, "ok");
      setTimeout(() => window.nativeClose(), 700);
    } catch (e) {
      toast("保存失败：" + (e && e.message || e), "error");
      saveBtn.disabled = false;
    }
  }
  saveBtn.addEventListener("click", save);
  input.addEventListener("keydown", e => {
    if (e.key === "Enter") save();
  });
}

// ─── Model page ───────────────────────────────────────────────────
async function initModelPage() {
  const f = {
    apiKey: document.getElementById("apiKey"),
    model:  document.getElementById("model"),
    host:   document.getElementById("host"),
    path:   document.getElementById("path"),
  };

  // Load current values
  try {
    const cur = await window.nativeReadSchema();
    if (cur) {
      f.apiKey.value = cur.api_key || "";
      f.model.value  = cur.model   || "deepseek-chat";
      f.host.value   = cur.host    || "api.deepseek.com";
      f.path.value   = cur.path    || "/v1/chat/completions";
    }
  } catch (e) {}

  // Preset buttons
  document.querySelectorAll(".preset").forEach(btn => {
    btn.addEventListener("click", () => {
      const p = PROVIDER_PRESETS[btn.dataset.preset];
      if (!p) return;
      f.model.value = p.model;
      f.host.value  = p.host;
      f.path.value  = p.path;
      toast("已套用 " + btn.textContent + " 预设。还需填 API Key。", "ok");
      f.apiKey.focus();
    });
  });

  // Show / hide API key
  document.getElementById("toggleKey").addEventListener("click", () => {
    f.apiKey.type = f.apiKey.type === "password" ? "text" : "password";
  });

  // Open get-key link
  document.getElementById("getKeyLink").addEventListener("click", () => {
    window.nativeOpenUrl("https://platform.deepseek.com/");
  });

  // Cancel
  document.getElementById("btnCancelModel").addEventListener("click", () => {
    window.nativeClose();
  });

  // Save
  document.getElementById("btnSaveModel").addEventListener("click", async () => {
    const ak = f.apiKey.value.trim();
    if (!ak) { toast("API Key 不能为空", "error"); return; }
    try {
      await window.nativeWriteSchema(ak, f.model.value.trim(),
                                     f.host.value.trim(), f.path.value.trim());
      toast("已保存，正在重启 WeaselServer ...", "ok");
      setTimeout(() => window.nativeClose(), 1100);
    } catch (e) {
      toast("保存失败：" + (e && e.message || e), "error");
    }
  });
}

// ─── Bootstrap ────────────────────────────────────────────────────
window.addEventListener("DOMContentLoaded", () => {
  const page = activePage();
  showPage(page);
  if (page === "model") initModelPage();
  else                  initLangPage();

  document.getElementById("btnClose").addEventListener("click",
    () => window.nativeClose());
  document.addEventListener("keydown", e => {
    if (e.key === "Escape") window.nativeClose();
  });
});
