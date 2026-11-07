@echo off
rem Configure + build Mario Tennis: Recompiled with clang-cl (VS 2026 Build Tools env) and Ninja.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%APPDATA%\Python\Python312\Scripts;%PATH%
cd /d F:\Recomps\MarioTennisRecomp
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MAKE_PROGRAM=ninja || exit /b 1
)
cmake --build build --target MarioTennisRecompiled -- -k 0 || exit /b 1
