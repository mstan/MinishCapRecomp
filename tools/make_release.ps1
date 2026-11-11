<#
make_release.ps1 - build the MinishCapRecomp windows release zip.

Modeled on the snesrecomp Zelda ALttP release script, adapted for the GBA
MinGW / CMake / Ninja toolchain (matches the sibling Pokemon repos' scripts):

  MinishCapRecomp-windows-x64-v<Version>.zip

Ships ONLY a zip (never a bare exe; the MinGW exe needs the bundled runtime
DLLs). The zip contains: MinishCapRecomp.exe (Release, MinGW, stripped) + the
four runtime DLLs (SDL2.dll, libgcc_s_seh-1.dll, libstdc++-6.dll,
libwinpthread-1.dll) + assets\ (the recomp-ui pre-boot launcher's fonts +
image TGAs, staged next to the exe by recomp_target_launcher_ui's POST_BUILD)
+ mods\ (the release-safe Adaptive Widescreen catalog) + README.md.

(Supersedes the older package_release.ps1, which built a static standalone exe;
this matches the dynamic + bundled-DLL layout shipped for every other GBA game.)

You supply your own legally-obtained ROM and a GBA BIOS dump (gba_bios.bin) on
first run - the runtime's native picker caches the chosen paths to rom.cfg /
bios.cfg next to the exe. Neither the ROM nor the BIOS is ever redistributed.

Zips land in release-stage\. Publish via gh AFTER the user signs off:

  gh release create v<Version> `
      release-stage\MinishCapRecomp-windows-x64-v<Version>.zip

Usage: powershell -File tools\make_release.ps1 -Version 0.0.2
#>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$BuildDir = 'build-release',
  [string]$OutputDir = 'release-stage',
  [string]$EngineRoot = '',
  [string]$RecompUiRoot = '',
  [string]$GeneratedBiosDir = '',
  [ValidateRange(1,32)][int]$Jobs = 2,
  [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'

$MingwBin = 'C:\msys64\mingw64\bin'
$env:PATH = "$MingwBin;$env:PATH"
$root  = Split-Path -Parent $PSScriptRoot
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $root $BuildDir }
$out = if ([IO.Path]::IsPathRooted($OutputDir)) { [IO.Path]::GetFullPath($OutputDir) } else { Join-Path $root $OutputDir }
if (-not $EngineRoot) { $EngineRoot=Join-Path $root 'gbarecomp' }
if (-not $RecompUiRoot) { $RecompUiRoot=Join-Path $root 'recomp-ui' }
$engine=(Resolve-Path -LiteralPath $EngineRoot).Path
$ui=(Resolve-Path -LiteralPath $RecompUiRoot).Path
if (-not $GeneratedBiosDir) { $GeneratedBiosDir=Join-Path $engine 'src/runtime/generated_bios' }
foreach ($entry in @(@('gbarecomp',$engine),@('recomp-ui',$ui))) {
  $pin=((git -C $root ls-tree HEAD $entry[0]) -split '\s+')[2]
  if ((git -C $entry[1] rev-parse HEAD).Trim() -ne $pin) { throw "$($entry[0]) differs from title pin $pin" }
  if (git -C $entry[1] status --porcelain --untracked-files=no --ignore-submodules=all) { throw "$($entry[0]) has uncommitted tracked changes" }
}
$Cmake=Join-Path $MingwBin 'cmake.exe'
New-Item -ItemType Directory -Force $out | Out-Null

# Single-game repo. game.toml is at the repo root (no variants/ layout).
$games = @(
  @{ Target = 'MinishCapRecomp'; Title = 'The Legend of Zelda: The Minish Cap'; Toml = (Join-Path $root 'game.toml') }
)
$dlls = @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')

# Configure once (Release; dynamic + bundled DLLs). GBARECOMP_ROOT defaults to
# ../gbarecomp (the engine checkout) via this repo's CMakeLists. SDL2 is located
# through the msys2 mingw64 prefix - passed explicitly so a fresh checkout
# configures without relying on auto-detection.
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
  & $Cmake -S $root -B $build -G Ninja `
      -DCMAKE_C_COMPILER="$MingwBin/cc.exe" `
      -DCMAKE_CXX_COMPILER="$MingwBin/c++.exe" `
      -DCMAKE_MAKE_PROGRAM="$MingwBin/ninja.exe" `
      -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG" `
      -DGBARECOMP_BUILD_ORACLE=OFF `
      "-DGBARECOMP_ROOT=$engine" "-DRECOMP_UI_ROOT=$ui" `
      "-DGBARECOMP_RUNTIME_UI_ROOT=$ui" "-DGBARECOMP_GENERATED_BIOS_DIR=$GeneratedBiosDir" `
      -DMINISH_ENABLE_ZELDA1_FOREIGN_WORLD=OFF `
      -DGBARECOMP_MINGW_PREFIX_UNIX="/c/msys64/mingw64" `
      -DSDL2_INCLUDE_DIR="C:/msys64/mingw64/include/SDL2" `
      -DSDL2_LIBRARY="C:/msys64/mingw64/lib/libSDL2.dll.a"
  if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
}
$hiddenGate = Select-String -Path (Join-Path $build 'CMakeCache.txt') `
    -SimpleMatch 'MINISH_ENABLE_ZELDA1_FOREIGN_WORLD:BOOL=OFF' | Select-Object -First 1
if (-not $hiddenGate) {
  throw 'release build must configure MINISH_ENABLE_ZELDA1_FOREIGN_WORLD=OFF'
}

foreach ($g in $games) {
  $target = $g.Target
  if (-not $SkipBuild) {
    & $Cmake --build $build --target $target --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "build failed for $target ($LASTEXITCODE)" }
  }

  $exe = Join-Path $build "$target.exe"
  if (-not (Test-Path $exe)) { throw "expected exe missing: $exe" }

  $stageName = "$target-windows-x64-v$Version"
  $stage = Join-Path $out $stageName
  if (Test-Path -LiteralPath $stage) { throw "Release stage already exists: $stage" }
  New-Item -ItemType Directory -Force $stage | Out-Null

  Copy-Item $exe $stage
  & "$MingwBin\strip.exe" (Join-Path $stage "$target.exe")
  if ($LASTEXITCODE -ne 0) { throw 'strip failed' }
  Copy-Item -LiteralPath (Join-Path $engine 'tools/CaptureDiagnostics.cmd') -Destination $stage
  foreach ($d in $dlls) { Copy-Item (Join-Path $MingwBin $d) $stage }

  # recomp-ui launcher assets (fonts + img TGAs), staged next to the exe by
  # recomp_target_launcher_ui's POST_BUILD. The launcher loads them from
  # <exe>\assets\ at runtime — a zip without them shows a blank launcher.
  $assets = Join-Path $build 'assets'
  if (-not (Test-Path (Join-Path $assets 'img'))) {
    throw "recomp-ui launcher assets missing: $assets (build with GBAGAME_RECOMP_UI=ON)"
  }
  Copy-Item $assets -Destination $stage -Recurse

  # The release-safe catalog contains only Adaptive Widescreen. The hidden
  # Zelda 1 checkpoint is staged exclusively by an explicit developer build.
  $mods = Join-Path $build 'mods'
  if (-not (Test-Path (Join-Path $mods 'packages'))) {
    throw "preloaded mod catalog missing: $mods"
  }
  $zeldaManifest = Get-ChildItem (Join-Path $mods 'packages') -Recurse `
      -File -Filter 'manifest.toml' | Select-String -SimpleMatch `
      'id = "minish-cap.foreign-world.zelda1"' | Select-Object -First 1
  if ($zeldaManifest) {
    throw "release catalog must not contain the hidden Zelda 1 checkpoint: $($zeldaManifest.Path)"
  }
  Copy-Item $mods -Destination $stage -Recurse

  # Bundle the self-contained tcc overlay toolchain (TinyCC + overlay shim
  # headers) next to the exe so a toolchain-less player box self-heals overlay
  # gaps via tcc (overlay backend auto -> tcc). See gbarecomp/tools/fetch_tcc.ps1.
  & (Join-Path $engine 'tools\fetch_tcc.ps1') -Toolchain (Join-Path $stage 'overlay_toolchain') -EngineRoot $engine -CacheDir (Join-Path $out '.toolchain-cache')

  # ROM SHA-1 from game.toml (best-effort; for the README only).
  $sha = ''
  if ($g.Toml -and (Test-Path $g.Toml)) {
    $m = Select-String -Path $g.Toml -Pattern '^\s*sha1\s*=\s*"([0-9a-fA-F]+)"' | Select-Object -First 1
    if ($m) { $sha = $m.Matches[0].Groups[1].Value }
  }

  @"
# $($g.Title) - GBA static recompilation (Windows x64)

Release build: an optimized native port. Running ``$target.exe`` opens the
pre-boot launcher: pick your ROM (and, on first run, your GBA BIOS under
Settings > System), tune display/audio/controls, then PLAY.

Static recompilation turns the game's ARM7TDMI code into native C++ (via the
[gbarecomp](https://github.com/mstan/gbarecomp) framework); the rest of the GBA
(PPU, APU, DMA, timers, BIOS HLE) runs through the framework's runner core.

## How to run

1. Extract this folder (keep the four DLLs plus the ``assets`` and ``mods``
   folders next to ``$target.exe``).
2. Run ``$target.exe``. The launcher opens: pick
   - your legally-obtained **$($g.Title) (USA)** ROM (``.gba``)$(if ($sha) { " - expected SHA-1 ``$sha``" })
   - a **GBA BIOS** dump (``gba_bios.bin``) under Settings > System.
   Settings, key rebinds, and hotkeys persist in ``config.ini`` /
   ``keybinds.ini`` next to the exe; the picked paths are cached to
   ``rom.cfg`` / ``bios.cfg``; save data lands next to your ROM.
   Fresh installations enable **Adaptive Widescreen**, following the window's
   aspect ratio. Existing saved mod choices are retained.
   (``--no-launcher`` boots straight to the game; "Skip launcher on boot"
   in the launcher does the same persistently, ``--launcher`` brings it back.)

The ROM and BIOS are **never** redistributed - supply your own dumps.

For a launcher or controller problem, run ``CaptureDiagnostics.cmd`` and close
the launcher normally. It writes ``launcher-diagnostics.log`` and
``controller-input.jsonl`` beside the game; no ROM is needed for controller capture.

See the GitHub release notes for what changed in v$Version.
"@ | Out-File (Join-Path $stage 'README.md') -Encoding utf8

  $zip = Join-Path $out "$stageName.zip"
  if (Test-Path -LiteralPath $zip) { throw "Release archive already exists: $zip" }
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem

  # ZIP entry names must always use '/', regardless of the host OS.
  # Compress-Archive preserves Windows backslashes, which POSIX extractors
  # treat as literal filename characters rather than directory separators -- so
  # a Linux / Steam Deck / Proton user gets files literally named
  # "assets\fonts\LatoLatin-Regular.ttf" (and "mods\packages\..." where the
  # game ships a mod catalog), the nested trees are never created, and the
  # ImGui launcher finds neither its fonts nor its mods. Write portably here,
  # then verify before anyone can publish it.
  # (Convention ported from snesrecomp/SuperMarioWorldRecomp.)
  $rzStageFull = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $stage).Path)
  $rzZipFull = [IO.Path]::GetFullPath($zip)
  $rzPrefix = $rzStageFull.TrimEnd('\') + '\'
  $rzFiles = @(Get-ChildItem -LiteralPath $stage -File -Recurse |
      Sort-Object FullName)
  $rzArchive = [IO.Compression.ZipFile]::Open(
      $rzZipFull, [IO.Compression.ZipArchiveMode]::Create)
  try {
      foreach ($rzFile in $rzFiles) {
          $rzFull = [IO.Path]::GetFullPath($rzFile.FullName)
          if (-not $rzFull.StartsWith(
                  $rzPrefix, [StringComparison]::OrdinalIgnoreCase)) {
              throw "Refusing to archive a file outside the release stage: $rzFull"
          }
          $rzName = $rzFull.Substring($rzPrefix.Length).Replace('\', '/')
          if ($rzName.StartsWith('/') -or $rzName -match '(^|/)..(/|$)') {
              throw "Unsafe ZIP entry name: $rzName"
          }
          [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
              $rzArchive, $rzFull, $rzName,
              [IO.Compression.CompressionLevel]::Optimal) | Out-Null
      }
  } finally {
      $rzArchive.Dispose()
  }

  # Read the archive back and reject non-portable entry names outright, so a
  # regression in the writer cannot ship a Windows-only zip again.
  $rzArchive = [IO.Compression.ZipFile]::OpenRead($rzZipFull)
  try {
      $rzBad = @($rzArchive.Entries | Where-Object {
          $_.FullName.Contains('\') -or
          $_.FullName.StartsWith('/') -or
          $_.FullName -match '(^|/)..(/|$)'
      })
      if ($rzBad.Count -ne 0) {
          throw "ZIP contains non-portable entry names: $(
              ($rzBad | ForEach-Object FullName) -join ', ')"
      }
      if ($rzArchive.Entries.Count -ne $rzFiles.Count) {
          throw "ZIP entry count mismatch: expected $($rzFiles.Count), got $(
              $rzArchive.Entries.Count)"
      }
  } finally {
      $rzArchive.Dispose()
  }
  Write-Host "--- $stageName ---"
  Get-ChildItem $stage | Select-Object Name, Length | Out-Host
  Get-Item $zip | Select-Object Name, Length | Out-Host
}
