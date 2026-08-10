# package_release.ps1 — Build a fully standalone MinishCapRecomp.exe.
#
# Static-links SDL2 + libstdc++ + libgcc + libwinpthread via the
# gbarecomp platform-core's GBARECOMP_STATIC_RELEASE=ON option, so
# the released binary has zero third-party DLL dependencies and ships with its
# release-safe built-in adaptive-view catalog, without a sidecar game.toml
# (the runtime's RunOptions defaults
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
        -DMINISH_ENABLE_ZELDA1_FOREIGN_WORLD=OFF `
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
}
$HiddenGate = Select-String -Path (Join-Path $BuildPath "CMakeCache.txt") `
    -SimpleMatch "MINISH_ENABLE_ZELDA1_FOREIGN_WORLD:BOOL=OFF" | Select-Object -First 1
if (-not $HiddenGate) {
    throw "release build must configure MINISH_ENABLE_ZELDA1_FOREIGN_WORLD=OFF"
}

cmake --build $BuildPath --target MinishCapRecomp

$BuiltExe = Join-Path $BuildPath "MinishCapRecomp.exe"
& "$MingwBin\strip.exe" $BuiltExe

if (Test-Path $ExeOut) {
    Remove-Item -Force $ExeOut
}
Copy-Item $BuiltExe $ExeOut

$BuiltMods = Join-Path $BuildPath "mods"
if (-not (Test-Path (Join-Path $BuiltMods "packages"))) {
    throw "preloaded mod catalog missing: $BuiltMods"
}
$ZeldaManifest = Get-ChildItem (Join-Path $BuiltMods "packages") -Recurse `
    -File -Filter "manifest.toml" | Select-String -SimpleMatch `
    'id = "minish-cap.foreign-world.zelda1"' | Select-Object -First 1
if ($ZeldaManifest) {
    throw "release catalog must not contain the hidden Zelda 1 checkpoint: $($ZeldaManifest.Path)"
}
$ModsOut = Join-Path $Root "mods"
New-Item -ItemType Directory -Force $ModsOut | Out-Null
$StaleZeldaPackage = Join-Path $ModsOut "packages\minish-cap.foreign-world.zelda1"
if (Test-Path $StaleZeldaPackage) {
    Remove-Item -LiteralPath $StaleZeldaPackage -Recurse -Force
}
Copy-Item (Join-Path $BuiltMods "*") -Destination $ModsOut -Recurse -Force

Write-Host ""
Write-Host "Built standalone: $ExeOut"
Get-Item $ExeOut | Format-List FullName, Length
Write-Host "Release mod catalog (adaptive view only): $ModsOut"

Write-Host ""
Write-Host "DLL imports (should be Windows system DLLs only):"
& "$MingwBin\objdump.exe" -p $ExeOut | Select-String 'DLL Name'
