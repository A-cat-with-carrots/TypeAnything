@echo on
cd /d "%~dp0"
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
set BOOST_ROOT=%~dp0deps\boost_1_84_0
set BJAM_TOOLSET=msvc-14.3
set PLATFORM_TOOLSET=v143
set "PATH=%PATH%;%USERPROFILE%\scoop\shims"
echo CWD=%CD%
echo BOOST_ROOT=%BOOST_ROOT%
if exist "%BOOST_ROOT%\boost" (
  echo BOOST SUBDIR FOUND
) else (
  echo BOOST SUBDIR MISSING
)
if exist "%BOOST_ROOT%\bootstrap.bat" (
  echo BOOTSTRAP FOUND at %BOOST_ROOT%\bootstrap.bat
) else (
  echo BOOTSTRAP MISSING
)
echo Test cd to BOOST_ROOT
cd /d "%BOOST_ROOT%"
echo CWD now=%CD%
echo Test direct call:
call .\bootstrap.bat
echo Bootstrap exit %ERRORLEVEL%
