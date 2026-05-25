# package_release.ps1 — Build a fully standalone MinishCapRecomp.exe.
#
# Static-links SDL2 + libstdc++ + libgcc + libwinpthread via the
# gbarecomp platform-core's GBARECOMP_STATIC_RELEASE=ON option, so
# the released binary has zero third-party DLL dependencies and ships
# without a sidecar game.toml (the runtime's RunOptions defaults
# carry the ROM SHA-1 + CRC32).
#
# Output: F:\Projects\gbarecomp\MinishCapRecomp\MinishCapRecomp.exe
#
# Usage:
#   .\tools\package_release.ps1
#   .\tools\package_release.ps1 -Version v0.0.1 -BuildDir build-release

param(
    [string]$Version  = "v0.0.1",
    [string]$BuildDir = "build-release"
)

$ErrorActionPreference = "Stop"

$Root      = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$ExeOut    = Join-Path $Root "MinishCapRecomp.exe"
$MingwBin  = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

if (-not (Test-Path (Join-Path $BuildPath "CMakeCache.txt"))) {
    cmake -S $Root -B $BuildPath -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DGBARECOMP_STATIC_RELEASE=ON `
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
}

cmake --build $BuildPath --target MinishCapRecomp

$BuiltExe = Join-Path $BuildPath "MinishCapRecomp.exe"
& "$MingwBin\strip.exe" $BuiltExe

if (Test-Path $ExeOut) {
    Remove-Item -Force $ExeOut
}
Copy-Item $BuiltExe $ExeOut

Write-Host ""
Write-Host "Built standalone: $ExeOut"
Get-Item $ExeOut | Format-List FullName, Length

Write-Host ""
Write-Host "DLL imports (should be Windows system DLLs only):"
& "$MingwBin\objdump.exe" -p $ExeOut | Select-String 'DLL Name'
