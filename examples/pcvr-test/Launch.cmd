@echo off
setlocal
set "ZSHARP_XR_UI_TRACE=1"
if not defined ZSHARP_LAB_VM (
    if exist "%~dp0..\..\build\Release\zsharp.exe" (
        set "ZSHARP_LAB_VM=%~dp0..\..\build\Release\zsharp.exe"
    ) else (
        set "ZSHARP_LAB_VM=zsharp"
    )
)
"%ZSHARP_LAB_VM%" package game "%~dp0." PCVR-Lab
if errorlevel 1 (
    pause
    exit /b 1
)
"%ZSHARP_LAB_VM%" open "%~dp0Packages\PCVR-Lab.zgame"
if errorlevel 1 pause
