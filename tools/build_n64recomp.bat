@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%APPDATA%\Python\Python312\Scripts;%PATH%
cmake -S F:\Recomps\src\N64Recomp -B F:\Recomps\src\N64Recomp\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
cmake --build F:\Recomps\src\N64Recomp\build --target N64RecompCLI RSPRecomp || exit /b 1
