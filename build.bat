@echo off
echo === Building PhotoFrame ===
echo.

echo [1/3] Clean previous builds...
dotnet clean PhotoFrame.sln -c Release -v q

echo.
echo [2/3] Publish self-contained...
dotnet publish src\PhotoFrame.UI\PhotoFrame.UI.csproj -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o publish\app

echo.
echo [3/3] Done!
echo.
echo Output: publish\app\PhotoFrame.UI.exe
echo.
echo To create installer, compile installer\setup.iss with Inno Setup.
pause
