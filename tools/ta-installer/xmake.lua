-- ta-installer: single-exe bundled installer for TypeAnything.
-- Embeds Weasel binaries + ta-settings + ui + schema yaml as RT_RCDATA.
--
-- The resource embedding is done OUTSIDE xmake by _stage_embed.ps1, which
-- runs rc.exe + cvtres.exe to produce embed/installer.res.obj (real COFF).
-- We just add that .obj to the link inputs here.

target("ta-installer")
  set_kind("binary")
  add_ldflags("/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup", {force = true})

  add_files("main.cpp")
  -- Prebuilt RES → COFF object that holds manifest + icon + RT_RCDATA blobs.
  add_files("embed/installer.res.obj")

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
