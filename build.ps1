# Builds Longshot Co-op with the llvm-mingw toolchain that comes with Longshot's builder (the same compiler as the game).
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Toolchain <llvm-mingw folder>] [-Install <game build folder>]
param(
    [string]$Toolchain = "",
    [string]$Install = "",
    [switch]$Release
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $Toolchain) {
    $candidates = @(
        "$env:USERPROFILE\Downloads\LongshotLauncher-data\toolchains",
        "$env:LOCALAPPDATA\LongshotLauncher\toolchains"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $tc = Get-ChildItem $c -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName "LongshotBuilder\tools\llvm-mingw" } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if ($tc) { $Toolchain = $tc; break }
        }
    }
}
if (-not (Test-Path "$Toolchain\bin\x86_64-w64-mingw32-clang++.exe")) { throw "llvm-mingw not found (pass -Toolchain)" }
$cc = "$Toolchain\bin\x86_64-w64-mingw32-clang.exe"
$cxx = "$Toolchain\bin\x86_64-w64-mingw32-clang++.exe"
$out = Join-Path $root "build"
$obj = Join-Path $out "obj"
New-Item -ItemType Directory -Force $out, $obj | Out-Null

function Run($exe, $argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "failed: $exe $($argv -join ' ')" }
}

# ---- the loader: winmm.dll
Push-Location $root
try { python -I tools/gen_winmm_proxy.py } finally { Pop-Location }
Run $cc @("-O2", "-shared", "-s", "-o", "$out\winmm.dll", "$root\src\proxy\winmm_proxy.c", "$root\src\proxy\winmm_stubs.S",
          "$root\src\proxy\winmm_exports.def", "-Wl,--enable-stdcall-fixup", "-luser32", "-lkernel32")

# ---- the mod: LongshotCoop.dll
$imguiDefs = @("-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS", "-DIMGUI_DISABLE_WIN32_FUNCTIONS", "-DIMGUI_DISABLE_DEMO_WINDOWS",
               "-DIMGUI_DISABLE_DEBUG_TOOLS", "-DNDEBUG")
$inc = @("-I$root\src\coop", "-I$root\third_party\imgui", "-I$root\third_party\minhook\include")
$cflags = @("-O2", "-g0", "-march=x86-64-v2", "-ffunction-sections", "-fdata-sections") + $imguiDefs + $inc
$objs = @()
# (third-party objects are kept between builds: rebuilt only when their source is newer)
$third = @(
    @("$root\third_party\minhook\src\hook.c", "c"),
    @("$root\third_party\minhook\src\buffer.c", "c"),
    @("$root\third_party\minhook\src\trampoline.c", "c"),
    @("$root\third_party\minhook\src\hde\hde64.c", "c"),
    @("$root\third_party\imgui\imgui.cpp", "cpp"),
    @("$root\third_party\imgui\imgui_draw.cpp", "cpp"),
    @("$root\third_party\imgui\imgui_widgets.cpp", "cpp"),
    @("$root\third_party\imgui\imgui_tables.cpp", "cpp")
)
foreach ($t in $third) {
    $src = $t[0]; $o = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($src) + ".o")
    if (-not (Test-Path $o) -or (Get-Item $src).LastWriteTime -gt (Get-Item $o).LastWriteTime) {
        if ($t[1] -eq "c") { Run $cc (@("-c", $src, "-o", $o) + $cflags) }
        else { Run $cxx (@("-c", "-std=c++20", $src, "-o", $o) + $cflags) }
    }
    $objs += $o
}
$srcs = Get-ChildItem "$root\src\coop" -Filter *.cpp
foreach ($s in $srcs) {
    $o = Join-Path $obj ("coop_" + $s.BaseName + ".o")
    Run $cxx (@("-c", "-std=c++20", "-Wall", "-Wno-unused-function", $s.FullName, "-o", $o) + $cflags)
    $objs += $o
}
$rc = "$root\src\coop\version.rc"
if (Test-Path $rc) {
    $ro = Join-Path $obj "version_res.o"
    Run "$Toolchain\bin\x86_64-w64-mingw32-windres.exe" @($rc, "-O", "coff", "-o", $ro)
    $objs += $ro
}
New-Item -ItemType Directory -Force "$out\coop" | Out-Null
Run $cxx (@("-shared", "-static", "-s", "-o", "$out\coop\LongshotCoop.dll") + $objs +
          @("-Wl,--gc-sections", "-lws2_32", "-liphlpapi", "-lshlwapi", "-luser32", "-lgdi32", "-lole32", "-lshell32", "-lbcrypt"))
Write-Host "built: $out\winmm.dll, $out\coop\LongshotCoop.dll"

if ($Install) {
    $bin = Join-Path $Install "bin"
    if (-not (Test-Path "$bin\longshot.exe")) { throw "no bin\longshot.exe in $Install" }
    Copy-Item "$out\winmm.dll" "$bin\winmm.dll" -Force
    New-Item -ItemType Directory -Force "$bin\coop" | Out-Null
    Copy-Item "$out\coop\LongshotCoop.dll" "$bin\coop\LongshotCoop.dll" -Force
    Write-Host "installed into $bin"
}
