@echo off
setlocal
cd /d "%~dp0.."
"C:\Program Files\dotnet\dotnet.exe" publish ".\BrirSpatializer.App\BrirSpatializer.App.csproj" -c Release -r win-x64 --self-contained false
endlocal

