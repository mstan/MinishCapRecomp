# package_release.ps1 — Stage + zip a Windows release of
# MinishCapRecomp.
#
# Builds MinishCapRecomp.exe via the existing CMake setup, stages it
# alongside its mingw64 runtime DLLs and the per-game config
# (game.toml + config/minishcap_usa.toml), drops a README +
# RELEASE_NOTES + LICENSE + START_HERE, and zips the result.
#
# Usage:
#   .\tools\package_release.ps1
#   .\tools\package_release.ps1 -Version v0.0.1 -BuildDir build

param(
    [string]$Version  = "v0.0.1",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

$Root      = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage     = Join-Path $StageRoot ("MinishCapRecomp-{0}-windows-x64" -f $Version)
$ZipPath   = Join-Path $Root ("MinishCapRecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin  = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

if (-not (Test-Path $BuildPath)) {
    cmake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release
}
cmake --build $BuildPath --target MinishCapRecomp

if (Test-Path $StageRoot) {
    Remove-Item -Recurse -Force $StageRoot
}
New-Item -ItemType Directory -Force $Stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "config") | Out-Null

# Binary.
Copy-Item (Join-Path $BuildPath "MinishCapRecomp.exe") $Stage

# Per-game config — game.toml + region overlay. The runner reads
# game.toml on launch; region overlay supplies the ROM SHA-1 + CRC32.
Copy-Item (Join-Path $Root "game.toml") $Stage
Copy-Item (Join-Path $Root "config\minishcap_usa.toml") (Join-Path $Stage "config\minishcap_usa.toml")

# Top-level docs.
Copy-Item (Join-Path $Root "README.md")        $Stage
Copy-Item (Join-Path $Root "LICENSE")          $Stage
Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage

# Runtime DLLs.
foreach ($Dll in @("SDL2.dll",
                   "libgcc_s_seh-1.dll",
                   "libstdc++-6.dll",
                   "libwinpthread-1.dll")) {
    $Source = Join-Path $MingwBin $Dll
    if (-not (Test-Path $Source)) {
        throw "Required runtime DLL not found: $Source"
    }
    Copy-Item $Source $Stage
}

@"
MinishCapRecomp $Version
========================

This release does NOT include the GBA BIOS or the Minish Cap ROM. You
provide your own legally obtained dumps:

  GBA BIOS:     gba_bios.bin
                16384 bytes
                SHA-1 300c20df6731a33952ded8c436f7f186d25d3492
                CRC32 0x21A2AE0A

  Minish Cap:   any-name.gba (USA / BZME)
                SHA-1 b4bd50e4131b027c334547b4524e2dbbd4227130
                CRC32 0x32D19810

First launch:
  1. Run MinishCapRecomp.exe.
  2. A file picker appears for the GBA BIOS. Select gba_bios.bin.
  3. A second picker appears for the Minish Cap (USA) ROM.
  4. Hashes are verified for both files. Mismatch -> warning dialog
     quoting actual + expected hashes; the runtime then attempts to
     boot anyway so atypical dumps can be tried.
  5. The recompiled BIOS plays the GAME BOY intro + chime, then the
     Minish Cap title screen renders.

Validated paths are remembered in bios.cfg and rom.cfg next to the
.exe. Delete them (or pass --bios / --rom) to pick again.

Default keymap:
  Z       = A           Arrow keys = D-pad
  X       = B           S          = R
  Return  = Start       A          = L
  R-Shift = Select      Esc        = quit

What works at v0.0.1:
  - BIOS intro byte-identical to mGBA on FB / PAL / VRAM / OAM.
  - Recompiled BIOS hands off into cart code.
  - Minish Cap title screen renders via recompiled-only execution.

What does NOT work yet:
  - Pressing Start at the title hits a code path the recompiler has
    not yet reached. The runtime aborts with a clear diagnostic
    (dispatch_miss / unimplemented_op) naming the gap.
  - Save / load, gameplay input, audio perceptual equality.

See RELEASE_NOTES.md for the full status and known issues. LICENSE is
PolyForm Noncommercial 1.0.0.
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "START_HERE.txt")

if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath

Write-Host ""
Write-Host "Staged: $Stage"
Write-Host "Zipped: $ZipPath"
Write-Host ""
Get-Item $ZipPath | Format-List FullName, Length
