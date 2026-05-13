@echo off
setlocal enabledelayedexpansion

@REM =================================================================================================
@REM = Chromeyumm Daily Runner
@REM = - Starts chromeyumm.exe and keeps it running
@REM = - Restarts automatically if the process crashes
@REM = - Restarts nightly between 1:00 AM and 2:00 AM
@REM = - Polls every 30 seconds to detect crashes quickly
@REM =================================================================================================

set "EXE_NAME=chromeyumm.exe"
set "EXE_PATH=%~dp0..\dist\chromeyumm.exe"
set "RESTART_HOUR_START=01"
set "RESTART_HOUR_END=02"
set "POLL_INTERVAL=30"
set "RESTART_COOLDOWN=10"

@REM Track whether we already did tonight's scheduled restart
set "DID_NIGHTLY_RESTART=0"

@REM =================================================================================================
@REM = :start
@REM = Launch chromeyumm and enter the monitoring loop
@REM =================================================================================================

:start
echo [%DATE% %TIME%] Starting %EXE_NAME% ...
start "" "%EXE_PATH%"

@REM Brief pause to let the process initialize
timeout /t 5 /nobreak > nul

@REM Verify it actually started
tasklist /fi "imagename eq %EXE_NAME%" 2>nul | find /i "%EXE_NAME%" > nul
if errorlevel 1 (
    echo [%DATE% %TIME%] ERROR: %EXE_NAME% failed to start. Retrying in %RESTART_COOLDOWN% seconds ...
    timeout /t %RESTART_COOLDOWN% /nobreak > nul
    goto start
)

echo [%DATE% %TIME%] %EXE_NAME% is running. Monitoring ...
goto monitor

@REM =================================================================================================
@REM = :monitor
@REM = Poll every POLL_INTERVAL seconds. Check:
@REM =   1. Is the process still alive? If not, restart immediately.
@REM =   2. Is it within the nightly restart window? If so, kill and restart.
@REM =================================================================================================

:monitor
timeout /t %POLL_INTERVAL% /nobreak > nul

@REM --- Check if process is still alive ---
tasklist /fi "imagename eq %EXE_NAME%" 2>nul | find /i "%EXE_NAME%" > nul
if errorlevel 1 (
    echo [%DATE% %TIME%] %EXE_NAME% has died! Restarting in %RESTART_COOLDOWN% seconds ...
    timeout /t %RESTART_COOLDOWN% /nobreak > nul
    goto start
)

@REM --- Check nightly restart window ---
set "HH=%time:~0,2%"
if "%HH:~0,1%"==" " set "HH=0%HH:~1,1%"

@REM Reset the nightly flag once we're outside the window
if %HH% GEQ %RESTART_HOUR_END% set "DID_NIGHTLY_RESTART=0"
if %HH% LSS %RESTART_HOUR_START% set "DID_NIGHTLY_RESTART=0"

@REM If inside the window and haven't restarted yet tonight, do it
if %HH% GEQ %RESTART_HOUR_START% if %HH% LSS %RESTART_HOUR_END% (
    if "!DID_NIGHTLY_RESTART!"=="0" (
        echo [%DATE% %TIME%] Nightly restart window reached. Restarting %EXE_NAME% ...
        set "DID_NIGHTLY_RESTART=1"
        goto kill_and_restart
    )
)

goto monitor

@REM =================================================================================================
@REM = :kill_and_restart
@REM = Gracefully kill the running process, wait, then restart
@REM =================================================================================================

:kill_and_restart
echo [%DATE% %TIME%] Stopping %EXE_NAME% ...
taskkill /im %EXE_NAME% /f > nul 2>&1

@REM Wait for process to fully exit
timeout /t %RESTART_COOLDOWN% /nobreak > nul

@REM Confirm it's gone
tasklist /fi "imagename eq %EXE_NAME%" 2>nul | find /i "%EXE_NAME%" > nul
if not errorlevel 1 (
    echo [%DATE% %TIME%] Process still running, force killing ...
    taskkill /im %EXE_NAME% /f /t > nul 2>&1
    timeout /t 5 /nobreak > nul
)

goto start
