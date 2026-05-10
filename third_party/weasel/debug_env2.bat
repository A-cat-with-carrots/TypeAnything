@echo off
cd /d "D:\hrdai\aiForType\third_party\weasel"
echo PATHEXT=%PATHEXT%
echo Try call .\env.bat:
call .\env.bat
echo BOOST_ROOT=[%BOOST_ROOT%]
echo Try call env (no ext):
call env
echo BOOST_ROOT=[%BOOST_ROOT%]
echo Try direct run env.bat:
env.bat
echo BOOST_ROOT=[%BOOST_ROOT%]
