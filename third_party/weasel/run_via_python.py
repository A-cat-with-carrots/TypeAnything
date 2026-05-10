"""Build pipeline — Phase A continuation.

Steps:
  1. Build librime deps (glog/leveldb/marisa/opencc/yaml-cpp) via CMake
  2. Build librime itself
"""

import os
import subprocess
import sys

WEASEL = r"D:\hrdai\aiForType\third_party\weasel"
LIBRIME = os.path.join(WEASEL, "librime")
BOOST_ROOT = r"C:\local\boost_1_84_0"  # prebuilt binaries
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"

clean_env = {
    "PATH": (
        r"C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;"
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer;"
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;"
        + os.environ["USERPROFILE"] + r"\scoop\shims"
    ),
    "SystemRoot": r"C:\Windows",
    "ComSpec": r"C:\Windows\System32\cmd.exe",
    "USERPROFILE": os.environ.get("USERPROFILE", r"C:\Users\12395"),
    "LOCALAPPDATA": os.environ.get("LOCALAPPDATA", r"C:\Users\12395\AppData\Local"),
    "APPDATA": os.environ.get("APPDATA", r"C:\Users\12395\AppData\Roaming"),
    "TEMP": os.environ.get("TEMP", r"C:\Windows\Temp"),
    "TMP": os.environ.get("TMP", r"C:\Windows\Temp"),
    "NUMBER_OF_PROCESSORS": "8",
    "PROCESSOR_ARCHITECTURE": os.environ.get("PROCESSOR_ARCHITECTURE", "AMD64"),
    "BOOST_ROOT": BOOST_ROOT,
    "BOOST_LIBRARYDIR": os.path.join(BOOST_ROOT, "lib64-msvc-14.3"),
    "VSINSTALLDIR": r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\\",
    "VCINSTALLDIR": r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\\",
    "VisualStudioVersion": "17.0",
}


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


# Step 1: librime deps (glog, leveldb, marisa, opencc, yaml-cpp)
log_deps = os.path.join(WEASEL, "_log_librime_deps.txt")
deps_script = f'''@echo off
call "{VCVARS}" x64
cd /d "{LIBRIME}"
set BOOST_ROOT={BOOST_ROOT}
set CMAKE_GENERATOR=Visual Studio 17 2022
set ARCH=x64
set PLATFORM_TOOLSET=v143
call .\\build.bat deps
'''
rc = run_bat("librime deps", deps_script, LIBRIME, log_deps)
if rc != 0:
    print(f"deps failed rc={rc}, see {log_deps}")
    sys.exit(rc)

# Step 2: librime itself (rime.dll)
log_rime = os.path.join(WEASEL, "_log_librime.txt")
rime_script = f'''@echo off
call "{VCVARS}" x64
cd /d "{LIBRIME}"
set BOOST_ROOT={BOOST_ROOT}
set CMAKE_GENERATOR=Visual Studio 17 2022
set ARCH=x64
set PLATFORM_TOOLSET=v143
call .\\build.bat librime
'''
rc = run_bat("librime", rime_script, LIBRIME, log_rime)
if rc != 0:
    print(f"librime failed rc={rc}, see {log_rime}")
    sys.exit(rc)

print("\n=== librime built. Next: add plugin + rebuild weasel ===")
