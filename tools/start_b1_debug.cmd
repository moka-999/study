@echo off
chcp 65001 >nul
setlocal
rem 启动已打包的验板窗口，不连接开发板、不自动开始循环。
set "B1_DEBUG_EXE=%~dp0..\build\b1-debug-tool\JMCT_B1_Debug.exe"
if not exist "%B1_DEBUG_EXE%" (
  echo 未找到验板工具，请先运行 tools\b1_debug\build.py 完成打包。
  pause
  exit /b 1
)
start "" "%B1_DEBUG_EXE%"
