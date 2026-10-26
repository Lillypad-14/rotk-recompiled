# -Target expanded plays the Expanded mod build (prepare/build it with --target expanded first).
# -CpuOverclock 1 plays at original GBA CPU speed (in-game slowdown included).
param(
    [ValidateSet('stock', 'expanded')][string]$Target = 'stock',
    [ValidateSet(1, 2, 4, 8, 16)][int]$CpuOverclock = 4
)
$ErrorActionPreference = 'Stop'
$env:GBARECOMP_CPU_OVERCLOCK = "$CpuOverclock"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Target -eq 'expanded') {
    $binary = Join-Path $projectRoot 'build/host-expanded/RotkPcLabExpanded.exe'
    $config = Join-Path $projectRoot '.private/expanded/game.toml'
    $playDirectory = Join-Path $projectRoot '.private/expanded/play'
    $save = 'rotk.sram'
} else {
    $binary = Join-Path $projectRoot 'build/host/RotkPcLab.exe'
    $config = Join-Path $projectRoot '.private/game.toml'
    $playDirectory = Join-Path $projectRoot '.private/play'
    $save = 'rotk.eeprom'
}
if (!(Test-Path -LiteralPath $binary) -or !(Test-Path -LiteralPath $config)) {
    throw "Build the project first: python tools/lab.py prepare --target $Target; python tools/lab.py build --target $Target"
}
New-Item -ItemType Directory -Force -Path $playDirectory | Out-Null
Push-Location $playDirectory
try {
    & $binary --config $config --window --save-path "$playDirectory/$save" --state-dir "$playDirectory/states"
    if ($LASTEXITCODE -ne 0) { throw "ROTK PC Lab exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}
