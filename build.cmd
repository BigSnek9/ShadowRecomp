@echo off
setlocal
call "%~dp0build_env.cmd" cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
if errorlevel 1 exit /b 1
call "%~dp0build_env.cmd" cmake --build "%~dp0build"
