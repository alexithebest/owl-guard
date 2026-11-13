@echo off
setlocal
where cmake >nul 2>&1
if errorlevel 1 (
  echo cmake nao encontrado no PATH.
  exit /b 1
)

cmake -S "%~dp0" -B "%~dp0build" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1

echo.
echo pronto: %~dp0build\Release\VantaGuard.exe
