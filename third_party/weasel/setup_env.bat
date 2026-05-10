@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
  echo VCVARSALL FAILED
  exit /b 1
)
echo ENV_OK
where cl
where msbuild
where cmake
