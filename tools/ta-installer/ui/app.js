// TypeAnything installer — UI logic + native bridge.
//
// Native bridge functions exposed by C++ (webview.bind):
//   nativeBeginInstall(apiKey, targetLang)  → kicks off the worker thread.
//      Progress / status streamed back via window.installerStatus(json).
//   nativeOpenUrl(url)
//   nativeClose()
//   nativeCancel()

function $(id) { return document.getElementById(id); }

function showPage(name) {
  document.querySelectorAll(".page").forEach(el => el.hidden = true);
  $("page-" + name).hidden = false;
  $("pageTitle").textContent =
    name === "key" ? "一键安装" :
    name === "progress" ? "安装中" :
    name === "done" ? "完成" : "";
}

function toast(msg, kind = "ok") {
  const el = $("toast");
  el.textContent = msg;
  el.className = "toast " + kind;
  el.hidden = false;
  requestAnimationFrame(() => el.classList.add("show"));
  setTimeout(() => {
    el.classList.remove("show");
    setTimeout(() => { el.hidden = true; }, 250);
  }, 1800);
}

function appendLog(msg, status) {
  const ul = $("log");
  const li = document.createElement("li");
  li.textContent = msg;
  if (status) li.classList.add(status);
  ul.appendChild(li);
  ul.scrollTop = ul.scrollHeight;
}

// Called by C++ via webview.eval each progress tick.
// payload = { percent: int, msg: string, log?: string, logStatus?: "done"|"error",
//             done?: bool, error?: string }
window.installerStatus = function (json) {
  let p;
  try { p = (typeof json === "string") ? JSON.parse(json) : json; } catch (e) { return; }
  if (typeof p.percent === "number") {
    $("bar").style.width = p.percent + "%";
  }
  if (p.msg) $("progressMsg").textContent = p.msg;
  if (p.log) appendLog(p.log, p.logStatus || null);
  if (p.error) {
    $("progressMsg").textContent = "安装失败：" + p.error;
    appendLog(p.error, "error");
  }
  if (p.done) {
    setTimeout(() => showPage("done"), 600);
  }
};

window.addEventListener("DOMContentLoaded", () => {
  // Default-target placeholder follows the same UX as ta-settings.
  // Toggle API key mask.
  $("toggleKey").addEventListener("click", () => {
    const t = $("apiKey");
    t.type = t.type === "password" ? "text" : "password";
  });

  $("getKeyLink").addEventListener("click", () => {
    window.nativeOpenUrl("https://platform.deepseek.com/");
  });

  $("btnCancel").addEventListener("click", () => window.nativeClose());

  $("btnStart").addEventListener("click", async () => {
    const ak = $("apiKey").value.trim();
    if (!ak) { toast("API Key 不能为空", "error"); return; }
    if (!/^sk-/.test(ak)) {
      // soft warning; still allow proceed since user might use a non-DeepSeek key.
      // do nothing — accept.
    }
    let tl = $("targetLang").value.trim() || "English";

    showPage("progress");
    $("bar").style.width = "2%";
    $("progressMsg").textContent = "准备中 …";

    try {
      await window.nativeBeginInstall(ak, tl);
    } catch (e) {
      $("progressMsg").textContent = "失败：" + (e && e.message || e);
      appendLog(String(e), "error");
    }
  });

  $("btnClose").addEventListener("click", () => window.nativeClose());

  document.addEventListener("keydown", e => {
    if (e.key === "Escape") {
      if (!$("page-progress").hidden) return; // don't allow cancel mid-install
      window.nativeClose();
    }
  });

  showPage("key");
});
