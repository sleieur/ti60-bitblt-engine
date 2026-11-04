<#
    load_game_assets.ps1 - push tools/game_assets.bin into DDR at 0x07400000

    The hand-painted sprite sheet (RUOK / Kenney, both CC0) is ~170 KiB, far more
    than the 124 KiB the program is linked into, so it lives in DDR next to the
    picture strip.  DDR holds its contents while the board is powered, so this
    runs once after power-up:

        1. program ddr_demo_ti60.bit
        2. .\load_game_assets.ps1     <- this script (pack + load)
        3. Debug / Run bbShow

    If it is skipped the game falls back to the procedural sprites: it still
    plays and still holds 60 fps, it just looks hand-drawn-free.

    Usage:  .\load_game_assets.ps1
            .\load_game_assets.ps1 -SkipRebuild
#>
param(
    [switch]$SkipRebuild,
    [int]$Speed = 16000
)

$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$bbShow = Split-Path -Parent $here
$bin    = Join-Path $here "game_assets.bin"

if (-not $SkipRebuild) {
    Write-Host "packing sprite sheet ..."
    & "C:\Users\coala\AppData\Local\Python\bin\python.exe" (Join-Path $here "pack_game.py")
    if ($LASTEXITCODE -ne 0) { throw "pack_game.py failed" }
}

if (-not (Test-Path -LiteralPath $bin)) { throw "game_assets.bin not found" }

$ocd = "D:\ws\Efinity RISC-V\openocd\bin\openocd.exe"
if (-not (Test-Path -LiteralPath $ocd)) {
    $cand = Get-ChildItem "D:\ws","C:\" -Filter "openocd.exe" -Recurse -Depth 5 -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "openocd" } | Select-Object -First 1
    if ($cand) { $ocd = $cand.FullName } else { throw "openocd.exe not found" }
}

$socRoot = (Resolve-Path -LiteralPath (Join-Path $bbShow "..\..\..\..")).Path
$ocdDir  = Join-Path $socRoot "bsp\efinix\EfxSapphireSoc\openocd"
$fdti = Join-Path $ocdDir "ftdi_ti.cfg"
$dbg  = Join-Path $ocdDir "debug_ti.cfg"

$size = (Get-Item -LiteralPath $bin).Length
Write-Host ("loading game_assets.bin ({0:N1} KiB) at 0x07400000 ..." -f ($size / 1KB))

$args2 = @(
    "-c", "set bsp_loc {$socRoot}",
    "-f", $fdti,
    "-f", $dbg,
    "-c", "adapter speed $Speed",
    "-c", "halt",
    "-c", "load_image {$bin} 0x07400000 bin",
    "-c", "echo {GAME ASSETS LOADED}",
    "-c", "shutdown"
)

$log = Join-Path $env:TEMP "load_game_assets.log"
$ErrorActionPreference = "Continue"
Push-Location $ocdDir
& $ocd @args2 *> $log
Pop-Location
$ErrorActionPreference = "Stop"

$txt = Get-Content -LiteralPath $log -ErrorAction SilentlyContinue
if ($txt -match "GAME ASSETS LOADED") {
    Write-Host "done - now Debug/Run bbShow; the game draws the hand-painted sprites" -ForegroundColor Green
} else {
    $txt | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }
    Write-Host "load failed - try -Speed 8000" -ForegroundColor Yellow
    exit 1
}
