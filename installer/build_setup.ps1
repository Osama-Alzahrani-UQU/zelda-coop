# Builds build\LongshotCoop-Setup.exe (the mod's installer) from the current build\winmm.dll and build\coop\LongshotCoop.dll.
#   powershell -ExecutionPolicy Bypass -File installer\build_setup.ps1 [-Toolchain <llvm-mingw folder>] [-SkipMod]
# The mod is built first (build.ps1) unless -SkipMod.
param(
    [string]$Toolchain = "",
    [switch]$SkipMod
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path $here -Parent
if (-not $Toolchain) {
    foreach ($c in @("$env:USERPROFILE\Downloads\LongshotLauncher-data\toolchains", "$env:LOCALAPPDATA\LongshotLauncher\toolchains")) {
        if (Test-Path $c) {
            $tc = Get-ChildItem $c -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName "LongshotBuilder\tools\llvm-mingw" } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if ($tc) { $Toolchain = $tc; break }
        }
    }
}
$cc = "$Toolchain\bin\x86_64-w64-mingw32-clang.exe"
$rc = "$Toolchain\bin\x86_64-w64-mingw32-windres.exe"
if (-not (Test-Path $cc)) { throw "llvm-mingw not found (pass -Toolchain)" }
if (-not $SkipMod) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1") -Toolchain $Toolchain
    if ($LASTEXITCODE -ne 0) { throw "the mod's build failed" }
}
foreach ($f in @("$root\build\winmm.dll", "$root\build\coop\LongshotCoop.dll")) {
    if (-not (Test-Path $f)) { throw "missing $f (build the mod first)" }
}
if (-not (Test-Path "$here\setup.ico")) {
    python -I "$here\make_icon.py" "$here\setup.ico"
    if ($LASTEXITCODE -ne 0) { throw "the icon could not be made" }
}
$obj = Join-Path $root "build\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
Push-Location $here
try {
    & $rc -O coff -I $here -o "$obj\setup_res.o" "$here\setup.rc"
    if ($LASTEXITCODE -ne 0) { throw "windres failed" }
} finally { Pop-Location }
$out = Join-Path $root "build\LongshotCoop-Setup.exe"
& $cc -O2 -s -municode -mwindows -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o $out "$here\setup.c" "$obj\setup_res.o" `
    -luser32 -lgdi32 -lshell32 -lole32 -luuid -lcomctl32 -static
if ($LASTEXITCODE -ne 0) { throw "the setup's build failed" }
Write-Host "built: $out ($([math]::Round((Get-Item $out).Length / 1KB)) KB)"
