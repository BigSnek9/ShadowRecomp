@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=%~dp0tools\bin;C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%PATH%"
rem keep a user conda out of CMake's package search (its abseil/zstd leak -ignore:4221 into clang links);
rem 'call set' expands %USERPROFILE% first, then removes that folder from PATH
call set "PATH=%%PATH:%USERPROFILE%\miniconda3\Scripts;=%%"
call set "PATH=%%PATH:%USERPROFILE%\miniconda3\Library\bin;=%%"
call set "PATH=%%PATH:%USERPROFILE%\miniconda3\Library\mingw-w64\bin;=%%"
call set "PATH=%%PATH:%USERPROFILE%\miniconda3\Library\usr\bin;=%%"
call set "PATH=%%PATH:%USERPROFILE%\miniconda3;=%%"
%*
