# build.ps1 -- builds main.dll against the installed UE4SS.dll
#
# Prerequisites:
#   * Visual Studio 2022 Build Tools (cl.exe / link.exe)
#   * UE4SS import library at %TEMP%\opencode\build\UE4SS.lib
#     (generated earlier from the installed UE4SS.dll's 4069 exports)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
$ErrorActionPreference = "Stop"

$root   = $PSScriptRoot
$src    = Join-Path $root "src"
$build  = Join-Path $root "build"
$inc    = Join-Path $root "UE4SSStubs"

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

$importLib = Join-Path $env:TEMP "opencode\build\UE4SS.lib"
if (-not (Test-Path $importLib)) {
    $importLib = Join-Path $build "UE4SS.lib"
}
if (-not (Test-Path $importLib)) {
    throw "UE4SS.lib not found. Generate it first (see README)."
}

if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

$sources = @(
    "config.cpp",
    "dispatch.cpp",
    "dllmain.cpp",
    "engine_hooks.cpp",
    "env_scan.cpp",
    "game_thread.cpp",
    "questdb.cpp",
    "RconMod.cpp",
    "server.cpp",
    "sqlite3.c",
    "synth.cpp"
)

# Build one batch command that runs inside the vcvars64 environment.
$cmd = "call `"$vcvars`"`r`ncd /d `"$build`"`r`n"
$objs = @()
foreach ($s in $sources) {
    $obj = ($s -replace '\.(c|cpp)$', '.obj')
    $objs += $obj
    $cmd += "cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /D_CRT_SECURE_NO_WARNINGS " +
            "/DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0601 /DUNICODE /D_UNICODE " +
            "/DSQLITE_THREADSAFE=1 /DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_DEFAULT_MEMSTATUS=0 " +
            "/I`"$inc`" /I`"$src`" /c `"$src\$s`" /Fo`"$obj`"`r`n"
}
$cmd += "link /nologo /DLL /OUT:main.dll $($objs -join ' ') `"$importLib`" ws2_32.lib`r`n"
$cmd += "if errorlevel 1 exit /b 1`r`necho BUILD_OK main.dll ready`r`n"

$bat = Join-Path $env:TEMP "opencode\scum_rcon_build.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
& cmd.exe /c "`"$bat`""

if (Test-Path (Join-Path $build "main.dll")) {
    Write-Output "SUCCESS: $(Join-Path $build 'main.dll')"
} else {
    Write-Output "FAILED: no main.dll produced"
    exit 1
}
