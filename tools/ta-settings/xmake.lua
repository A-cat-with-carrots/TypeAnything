-- ta-settings: WebView2 + HTML settings UI for TypeAnything tray menu items.
-- Builds an isolated win32 .exe that the IME spawns when user clicks
-- "切换语言" or "模型配置".

target("ta-settings")
  set_kind("binary")
  -- Win32 SUBSYSTEM:WINDOWS so the app has no console; wWinMain entry point.
  add_ldflags("/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup", {force = true})

  add_files("main.cpp")

  -- WebView2 SDK (vendored, x64 only).
  add_includedirs("vendor/webview2/include")
  add_linkdirs("vendor/webview2/x64")
  add_links("WebView2LoaderStatic")

  -- webview.h (zserge/webview, single-header) is included by main.cpp directly.

  add_syslinks(
    "user32", "shell32", "ole32", "advapi32", "dwmapi",
    "version", "shlwapi", "Userenv", "gdi32"
  )

  -- WinHTTP for any future net work; not strictly needed now.
  -- add_links("winhttp")

  set_languages("c++17")
  add_defines("UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "NOMINMAX")
  add_cxflags("/EHsc /utf-8")

  -- Copy UI assets next to the binary on build.
  after_build(function (target)
    local outdir = target:targetdir() .. "/ui"
    os.mkdir(outdir)
    os.cp("ui/*", outdir)
  end)
