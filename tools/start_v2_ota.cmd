@echo off
setlocal
chcp 65001 >nul
rem 仅启动交互菜单，不自动查询、升级或关闭电源。
py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul
if not errorlevel 1 (
    py -3 -B -X utf8 "%~dp0v2_ota.py"
    goto done
)
rem 本机已有独立Python运行时则直接复用，不要求操作人员安装Codex。
if exist "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" (
    "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" -B -X utf8 "%~dp0v2_ota.py"
    goto done
)
python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul
if not errorlevel 1 (
    python -B -X utf8 "%~dp0v2_ota.py"
    goto done
)
echo Python 3.10+ is required. Please install Python and enable the Python launcher.
:done
pause
