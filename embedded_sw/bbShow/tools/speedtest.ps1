<#
    speedtest.ps1 - measure the real JTAG write rate at several adapter speeds.

    The picture pipeline is limited by how fast the debugger can push bytes into
    DDR, not by DDR space or by the BitBlt engine.  This prints KB/s for a few
    adapter speeds so the frame budget of a video clip can be estimated.
#>
param(
    [string]$Speeds = "1000,2000,3000,5000",
    [int]$SizeKB = 2048
)

# note: $Speeds is typed [string] by the param block, so the parsed list has to
# live in a different variable - assigning an array to it would be re-stringified
$speedList = @($Speeds -split "[,\s]+" | Where-Object { $_ } | ForEach-Object { [int]$_ })

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bbShow = Split-Path -Parent $here
$socRoot = (Resolve-Path -LiteralPath (Join-Path $bbShow "..\..\..\..")).Path
$ocdDir = Join-Path $socRoot "bsp\efinix\EfxSapphireSoc\openocd"
$ocd = "D:\ws\Efinity RISC-V\openocd\bin\openocd.exe"

$tmp = Join-Path $env:TEMP "jtag_speed.bin"
$bytes = $SizeKB * 1024
$buf = New-Object byte[] $bytes
(New-Object Random 12345).NextBytes($buf)
[IO.File]::WriteAllBytes($tmp, $buf)
Write-Host ("payload: {0} KiB`n" -f $SizeKB)

$results = @()
foreach ($sp in $speedList) {
    $log = Join-Path $env:TEMP "speed_$sp.log"
    $ocdArgs = @(
        "-c", "set bsp_loc {$socRoot}",
        "-f", (Join-Path $ocdDir "ftdi_ti.cfg"),
        "-f", (Join-Path $ocdDir "debug_ti.cfg"),
        "-c", "adapter speed $sp",
        "-c", "halt",
        "-c", "load_image {$tmp} 0x06000000 bin",
        "-c", "echo {OK}",
        "-c", "shutdown"
    )
    Write-Host ("speed {0,5} kHz ... " -f $sp) -NoNewline
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $ErrorActionPreference = "Continue"
    Push-Location $ocdDir
    & $ocd @ocdArgs *> $log
    Pop-Location
    $ErrorActionPreference = "Stop"
    $sw.Stop()

    $ok = (Get-Content -LiteralPath $log -ErrorAction SilentlyContinue) -match "OK"
    $sec = $sw.Elapsed.TotalSeconds
    if ($ok) {
        $kbs = $SizeKB / $sec
        $perframe = 1800 / $kbs          # 1280x720 RGB565 = 1800 KiB
        Write-Host ("{0,7:N1} s   {1,7:N1} KB/s   {2,5:N1} s per frame" -f $sec, $kbs, $perframe)
        $results += [pscustomobject]@{ kHz = $sp; Seconds = $sec; KBps = $kbs; SecPerFrame = $perframe }
    } else {
        Write-Host ("FAILED ({0:N1} s)  - see $log" -f $sec)
    }
}

Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue

Write-Host ""
if ($results.Count -gt 0) {
    Write-Host "frames you can afford for a given load budget:"
    foreach ($r in $results) {
        $f10 = [int](600 / $r.SecPerFrame)
        $f30 = [int](1800 / $r.SecPerFrame)
        Write-Host ("  {0,5} kHz : 10 min -> {1,3} frames   30 min -> {2,3} frames" -f $r.kHz, $f10, $f30)
    }
}
