@echo off
rem 编译 kfm_chan_DLL.dll（MinGW-w64 64位）
set GCC=D:\mingw64\bin\gcc.exe
cd /d %~dp0
%GCC% -O2 -shared -o kfm_chan_DLL.dll kfm_chan_DLL.c kfm_chan_DLL.def -lm
if %errorlevel%==0 (echo BUILD OK) else (echo BUILD FAILED)
