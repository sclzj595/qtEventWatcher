@echo off
setlocal
chcp 65001 >nul
rem ============================================================
rem 打包.bat - QtEventWatcher Scout 卡顿检测工具包一键打包
rem   流程：构建(qt5 MSVC Release) -> 组装自包含产物 -> 完整性自检
rem         -> 冒烟(无 PATH 直启) -> zip 落盘 dist\
rem   用法：双击运行；自动化传 --no-pause 跳过末尾暂停
rem   产物内含全部 Qt 运行件（构建时 windeployqt 自动部署），零环境可解压即用
rem ============================================================

set "ROOT=%~dp0"
set "QT_BIN=F:\IDE.2\Qt5.15.2\5.15.2\msvc2019_64\bin"
set "PATH=%QT_BIN%;%PATH%"
set "BIN=%ROOT%build\bin\Release"
set "STAGE=%ROOT%build\package\QtEventWatcher-Scout"

echo [1/6] 构建检查...
if not exist "%ROOT%build\CMakeCache.txt" (
    echo   build 目录不存在，执行 CMake 配置（windows-msvc preset）...
    set "QT_EVENT_WATCHER_QT_DIR=%QT_BIN%\.."
    cmake --preset windows-msvc -S "%ROOT%" -B "%ROOT%build"
    if errorlevel 1 goto :fail
)

echo [2/6] 构建（增量，qt5 msvc Release）...
cmake --build "%ROOT%build" --config Release
if errorlevel 1 goto :fail

echo [3/6] 组装自包含产物...
for /f %%i in ('git -C "%ROOT%." describe --tags --abbrev^=0 2^>nul') do set "VER=%%i"
if "%VER%"=="" set "VER=unknown"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmm"') do set "STAMP=%%i"
set "ZIPNAME=QtEventWatcher-Scout-win64-%VER%-%STAMP%.zip"

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
for %%E in (scout.exe aggregator.exe scout-dashboard.exe basic_demo.exe) do (
    copy /y "%BIN%\%%E" "%STAGE%\" >nul || goto :fail
)
copy /y "%BIN%\*.dll" "%STAGE%\" >nul
for %%D in (platforms imageformats iconengines styles sqldrivers bearer tls res) do (
    if exist "%BIN%\%%D" robocopy "%BIN%\%%D" "%STAGE%\%%D" /e /nfl /ndl /njh /njs >nul
)
copy /y "%ROOT%scripts\dist\快速上手.md" "%STAGE%\" >nul
copy /y "%ROOT%scripts\dist\radar.ini" "%STAGE%\" >nul
copy /y "%ROOT%CHANGELOG.md" "%STAGE%\" >nul

echo [4/6] 完整性自检...
set "MISS="
if not exist "%STAGE%\Qt5Core.dll"        set "MISS=%MISS% Qt5Core.dll"
if not exist "%STAGE%\Qt5WebSockets.dll"  set "MISS=%MISS% Qt5WebSockets.dll"
if not exist "%STAGE%\Qt5Network.dll"     set "MISS=%MISS% Qt5Network.dll"
if not exist "%STAGE%\Qt5Sql.dll"         set "MISS=%MISS% Qt5Sql.dll"
if not exist "%STAGE%\platforms\qwindows.dll" set "MISS=%MISS% platforms\qwindows.dll"
if not exist "%STAGE%\sqldrivers\qsqlite.dll" set "MISS=%MISS% sqldrivers\qsqlite.dll"
if defined MISS (
    echo   [警告] 运行件缺失:%MISS%
    echo   （先单独跑一次 cmake --build 让 windeployqt 部署完成再打包）
    goto :fail
)
echo   [OK] Qt 运行件齐全

echo [5/6] 冒烟自检（无 PATH 模拟裸机直启）...
pushd "%STAGE%"
cmd /c "set PATH=C:\Windows\System32;C:\Windows&& scout.exe --badopt >nul 2>&1"
set "SMOKE=%errorlevel%"
popd
if not "%SMOKE%"=="2" (
    echo   [警告] scout.exe 直启异常 exit=%SMOKE%（预期 2=usage；非 2 多为缺 DLL）
    goto :fail
)
echo   [OK] scout.exe 裸机直启通过

echo [6/6] 压缩落盘 dist\%ZIPNAME% ...
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ROOT%dist\%ZIPNAME%' -Force"
if errorlevel 1 goto :fail
for %%Z in ("%ROOT%dist\%ZIPNAME%") do echo   [OK] %ZIPNAME%  %%~zZ 字节

echo.
echo ===== 打包完成 =====
echo   包: dist\%ZIPNAME%
echo   版本: %VER%（最新 tag）
echo   内容: scout / aggregator / scout-dashboard / basic_demo + Qt 运行件 + 快速上手
if "%1"=="--no-pause" exit /b 0
pause
exit /b 0

:fail
echo.
echo ===== 打包失败（见上方日志）=====
if "%1"=="--no-pause" exit /b 1
pause
exit /b 1
