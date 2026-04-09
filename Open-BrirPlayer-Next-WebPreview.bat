@echo off
setlocal

cd /d "%~dp0"

set "WEBUI_DIR=%~dp0BrirPlayer.Next.WebUI"
set "WEBUI_DIST=%WEBUI_DIR%\dist"
set "WEBUI_URL=http://127.0.0.1:4175/"

where npm >nul 2>nul
if errorlevel 1 (
    echo [ERROR] npm was not found.
    pause
    exit /b 1
)

if not exist "%WEBUI_DIR%\node_modules" (
    echo [INFO] Installing Web UI dependencies...
    pushd "%WEBUI_DIR%"
    call npm install
    if errorlevel 1 (
        popd
        echo [ERROR] npm install failed.
        pause
        exit /b 1
    )
    popd
)

echo [INFO] Building Web UI preview...
pushd "%WEBUI_DIR%"
call npm run build
if errorlevel 1 (
    popd
    echo [ERROR] Web UI build failed.
    pause
    exit /b 1
)
popd

if not exist "%WEBUI_DIST%\index.html" (
    echo [ERROR] Preview page was not found.
    pause
    exit /b 1
)

netstat -ano | find ":4175" | find "LISTENING" >nul
if errorlevel 1 (
    echo [INFO] Starting local Web UI server...
    powershell -NoProfile -WindowStyle Hidden -Command ^
        "$py=(Get-Command python -ErrorAction SilentlyContinue).Source; if(-not $py){$py=(Get-Command py -ErrorAction SilentlyContinue).Source}; if(-not $py){exit 1}; Start-Process -WindowStyle Hidden -FilePath $py -ArgumentList '-m','http.server','4175','--bind','127.0.0.1' -WorkingDirectory '%WEBUI_DIST%'" >nul 2>nul
    timeout /t 2 /nobreak >nul
)

echo [INFO] Opening preview page...
start "" "%WEBUI_URL%"

exit /b 0
