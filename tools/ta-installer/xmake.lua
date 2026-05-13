-- ta-installer: single-exe bundled installer for TypeAnything.
-- Embeds Weasel binaries + ta-settings + ui + schema yaml as RT_RCDATA.

target("ta-installer")
  set_kind("binary")
  -- /SUBSYSTEM:WINDOWS; wWinMain entry
  add_ldflags("/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup", {force = true})

  add_files("main.cpp", "installer.rc")

  -- Reuse ta-settings WebView2 SDK (vendored, x64).
  add_includedirs("../ta-settings/vendor/webview2/include")
  add_linkdirs("../ta-settings/vendor/webview2/x64")
  add_links("WebView2LoaderStatic")

  add_syslinks(
    "user32", "shell32", "ole32", "advapi32", "dwmapi",
    "version", "shlwapi", "Userenv", "gdi32"
  )

  set_languages("c++17")
  add_defines("UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "NOMINMAX")
  add_cxflags("/EHsc /utf-8")
