@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Install-RifeVulkan.ps1"
set "rife_install_result=%ERRORLEVEL%"
if not "%rife_install_result%"=="0" pause
exit /b %rife_install_result%
