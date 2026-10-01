@echo off
rem ADOFAI PERFECT — 一键注入（自动等待游戏与 Mono 就绪）
cd /d "%~dp0bin\Release"
if not exist Injector.exe (
    echo [!] 未找到 Injector.exe，请先用 VS2022 生成解决方案 ^(Release ^| x64^)
    pause
    exit /b 1
)
Injector.exe
pause
