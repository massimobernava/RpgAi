@echo off
setlocal

set INSTDIR=%~dp0
set WORKSPACE=%USERPROFILE%\Documents\RpgAi
set PORT=8080

:: Ensure workspace dirs exist
if not exist "%WORKSPACE%\scripts" mkdir "%WORKSPACE%\scripts"
if not exist "%WORKSPACE%\saves"   mkdir "%WORKSPACE%\saves"

:: Copy default scripts on first run
if not exist "%WORKSPACE%\scripts\fantasy_demo.lua" (
    xcopy /s /i /q "%INSTDIR%scripts\*" "%WORKSPACE%\scripts\" >nul 2>&1
)

:: First-run detection
set FIRST_RUN=0
if not exist "%WORKSPACE%\rpgai_settings.json" set FIRST_RUN=1

:: Launch engine
cd /d "%WORKSPACE%"
start "" "%INSTDIR%rpgai.exe" ^
    --web ^
    --path "%WORKSPACE%\scripts\" ^
    --save-path "%WORKSPACE%\saves\" ^
    --port %PORT%

:: Wait for server ready (poll up to 15 s)
set /a TRIES=0
:wait_loop
timeout /t 1 /nobreak >nul
curl -sf "http://localhost:%PORT%/" >nul 2>&1 && goto server_ready
set /a TRIES+=1
if %TRIES% lss 15 goto wait_loop

:server_ready
:: Open getting started on first run
if %FIRST_RUN%==1 (
    if exist "%INSTDIR%getting_started.html" (
        start "" "%INSTDIR%getting_started.html"
    )
)

start "" "http://localhost:%PORT%"

endlocal
