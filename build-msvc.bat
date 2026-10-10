@echo off
rem 在 Windows 上用 VS 2022 编译 x64 版 FileLocksmith
setlocal
where cmake >nul 2>nul || (echo 需要先安装 CMake 并加入 PATH & exit /b 1)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release
if errorlevel 1 exit /b 1
if not exist dist mkdir dist
copy /y build\Release\FileLocksmith.exe dist\ >nul
echo.
echo 完成: dist\FileLocksmith.exe
endlocal
