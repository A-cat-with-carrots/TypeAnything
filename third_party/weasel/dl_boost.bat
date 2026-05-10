@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
set RIME_ROOT=%CD%
set boost_version=1.84.0
set boost_x_y_z=1_84_0
set BOOST_ROOT=%RIME_ROOT%\deps\boost_%boost_x_y_z%

if exist "%BOOST_ROOT%\boost" (
  echo Boost already extracted at %BOOST_ROOT%
  exit /b 0
)

if not exist deps mkdir deps
echo Downloading boost %boost_version% via aria2c...
aria2c --max-tries=3 --retry-wait=5 --timeout=120 ^
  https://archives.boost.io/release/%boost_version%/source/boost_%boost_x_y_z%.7z ^
  -d %CD%\deps
if errorlevel 1 (
  echo aria2c download failed
  exit /b 1
)

echo Extracting...
pushd deps
7z x -y boost_%boost_x_y_z%.7z
popd

echo Boost ready at %BOOST_ROOT%
