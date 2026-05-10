@echo off
cd /d "D:\hrdai\aiForType\third_party\weasel"
echo Step 1: CWD=%CD%
echo Step 2: env.bat exists?
if exist env.bat echo YES env.bat present
if not exist env.bat echo NO env.bat MISSING
echo Step 3: contents of env.bat:
type env.bat
echo Step 4: calling env.bat
call env.bat
echo Step 5: BOOST_ROOT=%BOOST_ROOT%
echo Step 6: boost subdir exists?
if exist "%BOOST_ROOT%\boost" echo YES boost subdir found
if not exist "%BOOST_ROOT%\boost" echo NO boost subdir missing at %BOOST_ROOT%\boost
