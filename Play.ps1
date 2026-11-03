# -Target expanded plays the Expanded mod build (prepare/build it with --target expanded first).
# -Target ttt plays The Two Towers on the plain host (prepare/build it with --target ttt first).
# The launcher opens first: PC enhancements are on its Mods page, CPU boost included.
# -CpuOverclock N overrides the launcher's CPU boost for this run (1 = original GBA speed).
param(
    [ValidateSet('stock', 'expanded', 'ttt')][string]$Target = 'stock',
    [ValidateSet(0, 1, 2, 4, 8, 16)][int]$CpuOverclock = 0
)
$ErrorActionPreference = 'Stop'
if ($CpuOverclock -gt 0) { $env:GBARECOMP_CPU_OVERCLOCK = "$CpuOverclock" }
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Target -eq 'expanded') {
    $binary = Join-Path $projectRoot 'build/host-expanded/ROTK-Recompiled-Expanded.exe'
    $config = Join-Path $projectRoot '.private/expanded/game.toml'
    $playDirectory = Join-Path $projectRoot '.private/expanded/play'
    $save = 'rotk.sram'
} elseif ($Target -eq 'ttt') {
    $binary = Join-Path $projectRoot 'build/host-ttt/TTT-Recompiled.exe'
    $config = Join-Path $projectRoot '.private/ttt/game.toml'
    $playDirectory = Join-Path $projectRoot '.private/ttt/play'
    $save = 'ttt.eeprom'
} else {
    $binary = Join-Path $projectRoot 'build/host/ROTK-Recompiled.exe'
    $config = Join-Path $projectRoot '.private/game.toml'
    $playDirectory = Join-Path $projectRoot '.private/play'
    $save = 'rotk.eeprom'
}
if (!(Test-Path -LiteralPath $binary) -or !(Test-Path -LiteralPath $config)) {
    throw "Build the project first: python tools/lab.py prepare --target $Target; python tools/lab.py build --target $Target"
}
# The launcher's Expanded checkbox only works when its patch hash matches the Expanded build's ROM.
$cache = Join-Path $projectRoot 'build/host/CMakeCache.txt'
$expandedRom = Join-Path $projectRoot '.private/expanded/rotk.gba'
if ((Test-Path -LiteralPath $cache) -and (Test-Path -LiteralPath $expandedRom)) {
    $line = Select-String -LiteralPath $cache -Pattern '^ROTK_EDITION_SHA1:STRING=(.+)$' | Select-Object -First 1
    $built = (Get-FileHash -LiteralPath $expandedRom -Algorithm SHA1).Hash.ToLower()
    if ($line -and $line.Matches[0].Groups[1].Value -ne $built) {
        Write-Warning "The launcher's Expanded patch is stale, so ticking Expanded and pressing Play will do nothing. Fix: python tools/lab.py build --target expanded (it refreshes the launcher). See docs/EXPANDED_EDITION.md 'Play does nothing'."
    }
}
New-Item -ItemType Directory -Force -Path $playDirectory | Out-Null
Push-Location $playDirectory
try {
    & $binary --config $config --window --save-path "$playDirectory/$save" --state-dir "$playDirectory/states"
    if ($LASTEXITCODE -ne 0) { throw "ROTK Recompiled exited with code $LASTEXITCODE. The game's reason is printed above this error." }
} finally {
    Pop-Location
}
