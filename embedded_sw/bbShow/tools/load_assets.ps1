<#
    load_assets.ps1 - push tools/assets.bin into DDR at 0x06000000

    The pictures are 1.75 MiB each, far more than the 124 KiB the program is
    linked into, so they cannot be part of bbShow.elf.  They live in DDR
    instead.  DDR keeps its contents as long as the board is powered, so this
    only has to run once after power-up (and again whenever the pictures change):

        1. program ddr_demo_ti60.bit
        2. run this script            <- one time
        3. Debug / Run bbShow from the IDE as usual

    If you skip it the IMAGES scene simply says "NONE LOADED"; nothing else is
    affected.

    Speed: the FT4232 MPSSE saturates at about 171 KiB/s from 16 MHz up, so
    anything beyond 16000 kHz buys nothing.  Lower it if verification fails.

    Usage:  .\load_assets.ps1                    (pack + load + verify)
            .\load_assets.ps1 -SkipRebuild       (load the existing blob)
            .\load_assets.ps1 -Speed 8000        (slower, if verify fails)
            .\load_assets.ps1 -NoVerify          (skip the read-back check)
#>
param(
    [switch]$SkipRebuild,
    [int]$Speed = 16000,
    [string]$Assets,
    [switch]$NoVerify
)

$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$bbShow = Split-Path -Parent $here
if (-not $Assets) { $Assets = Join-Path $here "assets.bin" }

$ocd = "D:\ws\Efinity RISC-V\openocd\bin\openocd.exe"
if (-not (Test-Path -LiteralPath $ocd)) {
    $cand = Get-ChildItem "D:\ws","C:\" -Filter "openocd.exe" -Recurse -Depth 5 -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "openocd" } | Select-Object -First 1
    if ($cand) { $ocd = $cand.FullName } else { throw "openocd.exe not found" }
}

if (-not $SkipRebuild) {
    Write-Host "repacking pictures ..."
    & "C:\Users\coala\AppData\Local\Python\bin\python.exe" (Join-Path $here "img2assets.py")
    if ($LASTEXITCODE -ne 0) { throw "img2assets.py failed" }
}

if (-not (Test-Path -LiteralPath $Assets)) {
    Write-Host "no assets.bin - put pictures in tools\pics and run this again"
    exit 1
}

$size = (Get-Item -LiteralPath $Assets).Length
Write-Host ("loading {0} ({1:N1} KiB) at 0x06000000 ..." -f (Split-Path -Leaf $Assets), ($size / 1KB))
# 406 KiB/s measured at 16000 kHz - the FT4232 saturates there, so the raw JTAG
# bit rate is not the limit and must not be used for the estimate
Write-Host ("this takes roughly {0:N0} s ({1:N1} min) at {2} kHz" -f 
            ($size / 1KB / 406), ($size / 1KB / 406 / 60), $Speed)

# bbShow -> fb -> standalone -> software -> soc
$socRoot = (Resolve-Path -LiteralPath (Join-Path $bbShow "..\..\..\..")).Path
$ocdDir  = Join-Path $socRoot "bsp\efinix\EfxSapphireSoc\openocd"
$fdti = Join-Path $ocdDir "ftdi_ti.cfg"
$dbg  = Join-Path $ocdDir "debug_ti.cfg"
$bsp  = $socRoot

if (-not (Test-Path -LiteralPath $fdti)) { throw "ftdi_ti.cfg not found at $fdti" }

$ocdArgs = @(
    "-c", "set bsp_loc {$bsp}",
    "-f", $fdti,
    "-f", $dbg,
    "-c", "adapter speed $Speed",
    "-c", "halt",
    "-c", "load_image {$Assets} 0x06000000 bin",
    "-c", "echo {ASSETS LOADED}",
    "-c", "shutdown"
)

$sw  = [Diagnostics.Stopwatch]::StartNew()
$log = Join-Path $env:TEMP "load_assets.log"
$ErrorActionPreference = "Continue"          # openocd writes its banner to stderr
# debug_ti.cfg looks for cpu0.yaml relative to the working directory
Push-Location $ocdDir
& $ocd @ocdArgs *> $log
Pop-Location
$ErrorActionPreference = "Stop"
$sw.Stop()

$txt = Get-Content -LiteralPath $log -ErrorAction SilentlyContinue
$loaded = $false
if ($txt) {
    $loaded = [bool]($txt -match "ASSETS LOADED")
    $hit = $txt | Where-Object { $_ -match "ASSETS LOADED|rror|arn" }
    if ($hit) { $hit | ForEach-Object { Write-Host "  $_" } }
    elseif (-not $loaded) { $txt | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" } }
}
Write-Host ("written in {0:N1} s  ({1:N1} KiB/s)" -f $sw.Elapsed.TotalSeconds,
            ($size / 1KB / [Math]::Max(0.001, $sw.Elapsed.TotalSeconds)))

if (-not $loaded) {
    Write-Host ""
    Write-Host "the load did not report success - try a lower speed:" -ForegroundColor Yellow
    Write-Host "  .\load_assets.ps1 -SkipRebuild -Speed 8000"
    exit 1
}

# ---------------------------------------------------------------------------
# Read back three windows and compare.  load_image does not check anything, and
# at 16 MHz a marginal cable shows up as silently corrupted pixels, so a sample
# at the start, in the middle and at the end is worth the extra 3 seconds.
# ---------------------------------------------------------------------------
if (-not $NoVerify) {
    Write-Host ""
    Write-Host "verifying (reading back 3 windows) ..."
    $win = 64 * 1024
    $offsets = @(0, [int](($size / 2) - ($win / 2)), [int]($size - $win))
    $raw = [IO.File]::ReadAllBytes($Assets)

    $bad = 0
    foreach ($off in $offsets) {
        $back = Join-Path $env:TEMP ("verify_$off.bin")
        if (Test-Path -LiteralPath $back) { Remove-Item -LiteralPath $back -Force }
        $vArgs = @(
            "-c", "set bsp_loc {$bsp}",
            "-f", $fdti,
            "-f", $dbg,
            "-c", "adapter speed $Speed",
            "-c", "halt",
            "-c", ("dump_image {{{0}}} 0x{1:X8} 0x{2:X}" -f $back, (0x06000000 + $off), $win),
            "-c", "echo {VERIFY DONE}",
            "-c", "shutdown"
        )
        $vlog = Join-Path $env:TEMP "verify_$off.log"
        $ErrorActionPreference = "Continue"
        Push-Location $ocdDir
        & $ocd @vArgs *> $vlog
        Pop-Location
        $ErrorActionPreference = "Stop"

        if (-not (Test-Path -LiteralPath $back)) {
            Write-Host ("  offset 0x{0:X8}: read-back failed" -f $off) -ForegroundColor Yellow
            $bad++
            continue
        }
        $got = [IO.File]::ReadAllBytes($back)
        $n = [Math]::Min($win, [Math]::Min($got.Length, $raw.Length - $off))
        $diff = 0
        for ($i = 0; $i -lt $n; $i++) {
            if ($got[$i] -ne $raw[$off + $i]) { $diff++ }
        }
        if ($n -lt $win) {
            Write-Host ("  offset 0x{0:X8}: short read-back ({1} of {2})" -f $off, $n, $win) -ForegroundColor Yellow
            $bad++
        } elseif ($diff -eq 0) {
            Write-Host ("  offset 0x{0:X8}: {1} bytes identical" -f $off, $win) -ForegroundColor Green
        } else {
            Write-Host ("  offset 0x{0:X8}: {1} bytes DIFFER" -f $off, $diff) -ForegroundColor Red
            $bad++
        }
        Remove-Item -LiteralPath $back -Force -ErrorAction SilentlyContinue
    }

    if ($bad -gt 0) {
        Write-Host ""
        Write-Host ("verification FAILED in {0} of {1} windows." -f $bad, $offsets.Count) -ForegroundColor Red
        Write-Host "the frames will show corrupted blocks - reload at a lower speed:" -ForegroundColor Yellow
        Write-Host ("  .\load_assets.ps1 -SkipRebuild -Speed {0}" -f [Math]::Max(1000, [int]($Speed / 2)))
        exit 1
    }
    Write-Host "verification passed." -ForegroundColor Green
}

Write-Host ""
Write-Host "now Debug/Run bbShow from the IDE; scene 8 (IMAGES) plays the strip"
