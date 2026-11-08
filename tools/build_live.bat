@echo off
rem Configure + build the release (live recompilation) exe into build-live. Contains no game code.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%APPDATA%\Python\Python312\Scripts;%PATH%
cd /d F:\Recomps\MarioTennisRecomp
if not exist build-live\build.ninja (
  cmake -S . -B build-live -G Ninja -DMT_LIVE_RECOMP=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MAKE_PROGRAM=ninja || exit /b 1
)
rem librashader.dll (Slang shaders) built from F:\Recomps\src\librashader with "cargo xtask build --profile release".
if exist F:\Recomps\src\librashader\target\release\librashader.dll cmake -S . -B build-live -DLIBRASHADER_DLL=F:/Recomps/src/librashader/target/release/librashader.dll >nul
cmake --build build-live --target MarioTennisRecompiled -- -k 0 || exit /b 1
