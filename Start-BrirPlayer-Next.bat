@echo off
setlocal

cd /d "%~dp0"

set "APP_PROJECT=%~dp0BrirPlayer.Next.App\BrirPlayer.Next.App.csproj"
set "APP_EXE=%~dp0BrirPlayer.Next.App\bin\Debug\net8.0-windows\BrirPlayer.Next.App.exe"
set "APP_NAME=BrirPlayer.Next.App"

call :focus_app
if not errorlevel 1 (
    echo [INFO] BrirPlayer Next is already running. Bringing it to the front.
    exit /b 0
)

where dotnet >nul 2>nul
if errorlevel 1 (
    echo [ERROR] dotnet SDK was not found.
    pause
    exit /b 1
)

where npm >nul 2>nul
if errorlevel 1 (
    echo [ERROR] npm was not found.
    pause
    exit /b 1
)

echo [INFO] Building BrirPlayer Next...
dotnet build "%APP_PROJECT%" -c Debug
if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

if not exist "%APP_EXE%" (
    echo [ERROR] App executable was not found:
    echo %APP_EXE%
    pause
    exit /b 1
)

echo [INFO] Launching BrirPlayer Next...
start "" "%APP_EXE%"
timeout /t 2 /nobreak >nul
call :focus_app

exit /b 0

:focus_app
powershell -NoProfile -Command "$typeDef = 'using System; using System.Runtime.InteropServices; public static class Win32 { [DllImport(""user32.dll"")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow); [DllImport(""user32.dll"")] public static extern bool SetForegroundWindow(IntPtr hWnd); }'; Add-Type -TypeDefinition $typeDef; $p = Get-Process -Name '%APP_NAME%' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1; if (-not $p) { exit 1 }; [Win32]::ShowWindowAsync($p.MainWindowHandle, 9) | Out-Null; Start-Sleep -Milliseconds 150; [Win32]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; exit 0" >nul 2>nul
exit /b %errorlevel%
