// TypeAnything installer — UI logic + native bridge.
//
// Mode (set by C++ before app.js loads): window.TA_MODE = "install" | "uninstall".
//
// Native bridge:
//   nativeBeginInstall(dirUtf8)     → kicks off install worker. Progress
//                                     streamed via window.installerStatus(json).
//   nativeBeginUninstall()          → kicks off uninstall worker.
//   nativePickInstallDir()          → opens folder picker, returns path (or "").
//   nativeDefaultInstallDir()       → returns the prefill path for the picker.
//   nativeOpenSettings(page)        → spawns ta-settings.exe --page <model|lang>
//   nativeOpenUrl(url)              → ShellExecute the URL in default browser
//   nativeClose()

function $(id) { return document.getElementById(id); }

const MODE = (window.TA_MODE === "uninstall") ? "uninstall" : "install";

function showPage(name) {
  document.querySelectorAll(".page").forEach(el => el.hidden = true);
  const el = $("page-" + name);
  if (el) el.hidden = false;
  $("pageTitle").textContent =
    name === "progress" ? (MODE === "uninstall" ? "卸载中" : "安装中")
  : name === "done"     ? "完成"
  :                       "";
}

function appendLog(msg, status) {
  const ul = $("log");
  const li = document.createElement("li");
  li.textContent = msg;
  if (status) li.classList.add(status);
  ul.appendChild(li);
  ul.scrollTop = ul.scrollHeight;
}

// Called by C++ on each progress tick.
window.installerStatus = function (json) {
  let p;
  try { p = (typeof json === "string") ? JSON.parse(json) : json; } catch (e) { return; }
  if (typeof p.percent === "number") $("bar").style.width = p.percent + "%";
  if (p.msg)   $("progressMsg").textContent = p.msg;
  if (p.log)   appendLog(p.log, p.logStatus || null);
  if (p.error) {
    const verb = MODE === "uninstall" ? "卸载" : "安装";
    $("progressMsg").textContent = verb + "失败：" + p.error;
    appendLog(p.error, "error");
  }
  if (p.done)  { setTimeout(() => showPage("done"), 500); }
};

let installDir = "";

async function startInstall() {
  showPage("progress");
  $("bar").style.width = "2%";
  $("progressMsg").textContent = "准备中 …";
  try { await window.nativeBeginInstall(installDir); }
  catch (e) {
    $("progressMsg").textContent = "失败：" + (e && e.message || e);
    appendLog(String(e), "error");
  }
}

async function startUninstall() {
  showPage("progress");
  $("progressMsg").textContent = "准备中 …";
  $("bar").style.width = "2%";
  // Repaint done page text for uninstall flow.
  const doneTitle = document.querySelector("#page-done h1");
  const doneSub = document.querySelector("#page-done .sub");
  const doneLegal = document.querySelector("#page-done .legal");
  if (doneTitle) doneTitle.innerHTML = "卸载完成。";
  if (doneSub)   doneSub.textContent = "TypeAnything 已从系统移除。用户字典与个人配置已保留。";
  if (doneLegal) doneLegal.remove();
  const btnPrimary = $("btnOpenModel");
  const btnSkip    = $("btnSkip");
  if (btnPrimary) btnPrimary.remove();
  if (btnSkip) {
    btnSkip.textContent = "关闭";
    btnSkip.classList.remove("ghost");
    btnSkip.classList.add("primary");
  }
  try { await window.nativeBeginUninstall(); }
  catch (e) {
    $("progressMsg").textContent = "失败：" + (e && e.message || e);
    appendLog(String(e), "error");
  }
}

window.addEventListener("DOMContentLoaded", async () => {
  $("btnSkip").addEventListener("click", () => window.nativeClose());
  $("btnOpenModel").addEventListener("click", async () => {
    try { await window.nativeOpenSettings("model"); } catch (e) {}
    setTimeout(() => window.nativeClose(), 400);
  });

  if (MODE === "uninstall") {
    $("btnUninstCancel").addEventListener("click", () => window.nativeClose());
    $("btnUninstStart").addEventListener("click", startUninstall);
    document.addEventListener("keydown", e => {
      if (e.key === "Escape" && $("page-progress").hidden) {
        window.nativeClose();
      }
    });
    showPage("uninstall-welcome");
    return;
  }

  // Install mode.
  $("btnCancel").addEventListener("click", () => window.nativeClose());
  $("btnStart").addEventListener("click", startInstall);
  $("btnPickDir").addEventListener("click", async () => {
    try {
      const picked = await window.nativePickInstallDir();
      if (picked) {
        installDir = picked;
        $("installDirPath").textContent = picked;
        $("installDirPath").title = picked;
      }
    } catch (e) {}
  });

  // Prefill default install dir.
  try {
    const def = await window.nativeDefaultInstallDir();
    if (def) {
      installDir = def;
      $("installDirPath").textContent = def;
      $("installDirPath").title = def;
    }
  } catch (e) {}

  document.addEventListener("keydown", e => {
    if (e.key === "Enter" && !$("page-welcome").hidden) {
      e.preventDefault();
      startInstall();
      return;
    }
    if (e.key === "Escape") {
      if (!$("page-progress").hidden) return;
      window.nativeClose();
    }
  });

  showPage("welcome");
});
