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
  // Tab visual state.
  document.querySelectorAll(".page-tab").forEach(b => {
    b.classList.toggle("active", b.dataset.page === name);
  });
}

// Initialize model page lazily — only when user clicks the tab the first time,
// to avoid two nativeRead* calls at startup.
let _modelInited = false;
async function ensureModelInit() {
  if (_modelInited) return;
  _modelInited = true;
  try { await initModelPage(); } catch (e) { /* surface via toast inside */ }
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

  // Pre-fill only if user has a non-default custom value. The install
  // script seeds "en" (legacy 2-letter code that maps to English), and
  // "English" is also the implicit default, so both should leave the
  // field blank so the placeholder "English" shows in muted tone.
  // Clicking puts the cursor at the leftmost position — nothing to delete.
  const DEFAULT_ALIASES = new Set(["en", "english", "english "]);
  let currentLangValue = "";
  try {
    const cur = await window.nativeReadLang();
    currentLangValue = (cur || "").trim();
    if (currentLangValue && !DEFAULT_ALIASES.has(currentLangValue.toLowerCase())) {
      // Show "无" instead of "off" — friendlier UI label.
      input.value = currentLangValue.toLowerCase() === "off"
                      ? "无"          // 无
                      : currentLangValue;
    }
  } catch (e) {}

  // 纯净模式 toggle: lives in the titlebar (top right). When ON, write
  // "off" to the lang file and grey out the rest of the panel so it's
  // visually obvious that translation is disabled. When OFF, restore the
  // previously typed value (or "English") and unlock the UI.
  const pureToggle = document.getElementById("pureToggle");
  const mainEl = document.querySelector("main");
  let lastLang = "";  // remembers value before pure mode flip-on
  function applyPureLock(on) {
    if (mainEl) {
      mainEl.classList.toggle("pure-locked", !!on);
    }
    // Disable form controls in BOTH pages (lang chips/save + model
    // form/preset/save) so keyboard tabbing can't reach them either.
    document.querySelectorAll(
      "#page-lang input, #page-lang button.chip, #page-lang .btn.primary, " +
      "#page-model input, #page-model button"
    ).forEach(el => { el.disabled = !!on; });
  }
  if (pureToggle) {
    // Initial state: reflect current lang file.
    try {
      const cur = (typeof currentLangValue === "string" ? currentLangValue : "");
      if (cur.toLowerCase() === "off") {
        pureToggle.checked = true;
        applyPureLock(true);
      }
    } catch (e) {}
    pureToggle.addEventListener("change", async () => {
      if (pureToggle.checked) {
        const cur = input.value.trim();
        if (cur && cur !== "无") lastLang = cur;
        input.value = "无";  // 无 — friendly display
        applyPureLock(true);
        try { await window.nativeWriteLang("off"); } catch (e) {}
      } else {
        const restore = lastLang || input.placeholder || "English";
        input.value = restore;
        applyPureLock(false);
        try { await window.nativeWriteLang(restore); } catch (e) {}
      }
    });
  }

  // Click chip → fill input.
  function clearChipActive() {
    document.querySelectorAll(".chip.active").forEach(x => x.classList.remove("active"));
  }
  document.querySelectorAll(".chip").forEach(c => {
    c.addEventListener("click", () => {
      input.value = c.dataset.value || c.textContent;
      input.focus();
      const n = input.value.length;
      input.setSelectionRange(n, n);
      clearChipActive();
      c.classList.add("active");
    });
  });
  // Any manual edit invalidates the chip "selected" state.
  input.addEventListener("input", clearChipActive);
  input.addEventListener("focus", () => {
    const v = input.value.trim();
    let match = null;
    document.querySelectorAll(".chip").forEach(c => {
      const cv = (c.dataset.value || c.textContent || "").trim();
      if (cv === v) match = c;
    });
    if (!match) clearChipActive();
  });

  // Save: write lang.txt + close. Empty input falls back to the placeholder
  // default ("English"), so user can just press Save without typing anything.
  // Display "无" for the "off" sentinel; keep the file's stored value
  // ASCII so the plugin's compare stays codepage-safe.
  const PURE_DISPLAY = "无";          // 无
  function toStoredLang(v) {
    return (v && v.trim() === PURE_DISPLAY) ? "off" : v;
  }
  function fromStoredLang(v) {
    return (v && v.trim().toLowerCase() === "off") ? PURE_DISPLAY : v;
  }

  async function save() {
    let v = input.value.trim();
    if (!v) v = input.placeholder || "English";
    v = toStoredLang(v);
    saveBtn.disabled = true;
    try {
      await window.nativeWriteLang(v);
      toast("已保存", "ok");
      // Stay open — user may want to make more changes. They close with X.
    } catch (e) {
      toast("保存失败：" + (e && e.message || e), "error");
    } finally {
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

  // Live endpoint preview — show full URL the request will hit.
  const ep = document.getElementById("endpointPreview");
  function refreshPreview() {
    if (!ep) return;
    const h = (f.host.value || "").trim();
    const p = (f.path.value || "").trim();
    if (!h && !p) { ep.classList.add("empty"); ep.textContent = ""; return; }
    const scheme = /^localhost(:|$)|^127\.|^\d+\.\d+\.\d+\.\d+/.test(h) ? "http" : "https";
    const pp = p.startsWith("/") ? p : ("/" + p);
    ep.classList.remove("empty");
    ep.textContent = "POST  " + scheme + "://" + h + pp;
  }
  refreshPreview();
  f.host.addEventListener("input", refreshPreview);
  f.path.addEventListener("input", refreshPreview);

  // Preset buttons
  document.querySelectorAll(".preset").forEach(btn => {
    btn.addEventListener("click", () => {
      const p = PROVIDER_PRESETS[btn.dataset.preset];
      if (!p) return;
      f.model.value = p.model;
      f.host.value  = p.host;
      f.path.value  = p.path;
      refreshPreview();
      f.apiKey.focus();
      document.querySelectorAll(".preset").forEach(x => x.classList.remove("active"));
      btn.classList.add("active");
    });
  });

  // Show / hide API key
  document.getElementById("toggleKey").addEventListener("click", () => {
    f.apiKey.type = f.apiKey.type === "password" ? "text" : "password";
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
window.addEventListener("DOMContentLoaded", async () => {
  // Always init both pages so user can swap freely. Lang init is cheap;
  // model init makes one nativeReadSchema call.
  await initLangPage();
  await initModelPage();
  _modelInited = true;

  // Tab nav.
  document.querySelectorAll(".page-tab").forEach(b => {
    b.addEventListener("click", () => showPage(b.dataset.page));
  });

  const page = activePage();
  showPage(page);

  document.addEventListener("keydown", e => {
    if (e.key === "Escape") window.nativeClose();
  });
});
