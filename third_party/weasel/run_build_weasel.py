"""Build Weasel UI components (TSF, Server, Deployer, etc) via msbuild.

Pre-reqs:
  - librime already built (rime.dll in librime/dist/lib/)
  - boost prebuilt at C:\\local\\boost_1_84_0
  - source files patched with TypeAnything branding
"""
import os, subprocess, sys, shutil

WEASEL = r"D:\hrdaiiForType\third_party\weasel"
LIBRIME_DIST = os.path.join(WEASEL, "librime", "dist")
BOOST_ROOT = r"C:\local\boost_1_84_0"
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

# Inherit user env (msbuild needs many SpecialFolder/registry vars) but strip
# MSYS / Git Bash entries from PATH that confuse cl.exe link.
def _build_env():
    e = dict(os.environ)
    # Strip MSYS Git's bin entries (they shadow link.exe etc.)
    raw = e.get("PATH", "")
    keep = []
    for part in raw.split(";"):
        low = part.lower()
        if any(x in low for x in (r"\git\mingw", r"\git\usr", r"git\bin", r"\msys", r"\anaconda3\library\mingw", r"\anaconda3\library\usr")):
            continue
        keep.append(part)
    # Prepend our build tool dirs
    prepend = [
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
        os.path.expanduser(r"~\scoop\shims"),
    ]
    e["PATH"] = ";".join(prepend + keep)
    e["BOOST_ROOT"] = BOOST_ROOT
    e["BOOST_LIBRARYDIR"] = os.path.join(BOOST_ROOT, "lib64-msvc-14.3")
    e["VSINSTALLDIR"] = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\\"
    e["VCINSTALLDIR"] = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\\"
    e["VisualStudioVersion"] = "17.0"
    e["NUMBER_OF_PROCESSORS"] = e.get("NUMBER_OF_PROCESSORS", "8")
    return e


clean_env = _build_env()


def run_bat(label, script, cwd, log_path):
    path = os.path.join(cwd, "_run_step.bat")
    with open(path, "w", encoding="ascii", newline="\r\n") as f:
        f.write(script)
    print(f"\n=== {label} ===  cwd={cwd}\nlog: {log_path}")
    sys.stdout.flush()
    with open(log_path, "wb") as logf:
        proc = subprocess.run(
            ["cmd.exe", "/D", "/C", path],
            cwd=cwd,
            env=clean_env,
            shell=False,
            stdout=logf,
            stderr=subprocess.STDOUT,
        )
    print(f"=== {label} exit {proc.returncode} ===")
    sys.stdout.flush()
    return proc.returncode


# librime lib + dll need to be where Weasel's projects expect them:
#   weasel/output/  (rime.dll, bin)
#   weasel/lib64/   (rime.lib for x64)
#   weasel/lib/     (rime.lib for x86 — we don't have x86; symlink x64 copy)
output = os.path.join(WEASEL, "output")
lib64 = os.path.join(WEASEL, "lib64")
os.makedirs(output, exist_ok=True)
os.makedirs(lib64, exist_ok=True)

# Copy librime dist files
for name in ("rime.dll", "rime.lib", "rime.pdb"):
    src = os.path.join(LIBRIME_DIST, "lib", name)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(output, name))
        print(f"  copied {name} -> output/")
        if name == "rime.lib":
            shutil.copy2(src, os.path.join(lib64, name))
# Copy headers
include_src = os.path.join(LIBRIME_DIST, "include")
include_dst = os.path.join(WEASEL, "include")
if os.path.exists(include_src):
    for f in os.listdir(include_src):
        s = os.path.join(include_src, f)
        d = os.path.join(include_dst, f)
        if os.path.isdir(s):
            if os.path.exists(d): shutil.rmtree(d)
            shutil.copytree(s, d)
        else:
            shutil.copy2(s, d)
    print(f"  copied librime headers -> include/")


# Render weasel.props from template
log_props = os.path.join(WEASEL, "_log_weasel_props.txt")
props_script = '''@echo off
cd /d "%s"
cscript.exe //nologo render.js weasel.props BOOST_ROOT PLATFORM_TOOLSET VERSION_MAJOR VERSION_MINOR VERSION_PATCH PRODUCT_VERSION FILE_VERSION
''' % WEASEL
clean_env["VERSION_MAJOR"] = "0"
clean_env["VERSION_MINOR"] = "17"
clean_env["VERSION_PATCH"] = "4"
clean_env["PRODUCT_VERSION"] = "0.17.4.0"
clean_env["FILE_VERSION"] = "0.17.4.0"
clean_env["PLATFORM_TOOLSET"] = "v143"
rc = run_bat("render weasel.props", props_script, WEASEL, log_props)
if rc != 0:
    print(f"render failed rc={rc}")
    sys.exit(rc)


# msbuild weasel.sln Release x64
log_msbuild = os.path.join(WEASEL, "_log_weasel_msbuild.txt")
msbuild_script = f'''@echo off
call "{VCVARS}" x64
cd /d "{WEASEL}"
msbuild.exe weasel.sln /m /p:Configuration=Release /p:Platform=x64 /p:TrackFileAccess=false /p:UseHardlinksIfPossible=false /fl /flp:LogFile=msbuild.x64.log
'''
rc = run_bat("msbuild weasel x64", msbuild_script, WEASEL, log_msbuild)
if rc != 0:
    print(f"msbuild failed rc={rc}")
    print("Last 50 lines of log:")
    with open(log_msbuild, "rb") as f:
        text = f.read().decode("gbk", errors="replace")
    for line in text.split("\n")[-50:]:
        try:
            print("  " + line.rstrip())
        except UnicodeEncodeError:
            print("  [non-ascii line]")
    sys.exit(rc)

print("\n=== weasel UI build done ===")
print("Outputs in", output)
