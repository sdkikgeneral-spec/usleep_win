@echo off
setlocal

set SCRIPT_DIR=%~dp0
powershell -ExecutionPolicy Bypass -File "%SCRIPT_DIR%tools\meson_build_msvc.ps1" %*

exit /b %ERRORLEVEL%
