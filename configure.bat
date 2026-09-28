@echo off
setlocal

cd /d "%~dp0"

echo ============================================================
echo   QtEventWatcher - CMake Configure
echo ============================================================
echo.

echo [1/3] Cleaning build directory...

if exist build (
    rmdir /s /q build
)

echo [OK] Build directory cleaned.
echo.

echo [2/3] Configuring CMake...

cmake -S . -B build -G "Visual Studio 16 2019" -A x64

if errorlevel 1 (
    echo.
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo [OK] CMake configuration completed.
echo.

echo [3/3] Build directory:
echo %CD%\build
echo.

echo ============================================================
echo   Configure completed successfully.
echo ============================================================
echo.

pause
exit /b 0