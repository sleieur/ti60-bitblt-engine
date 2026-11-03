<#
    game_freeze.ps1 - jump to the game scene, wait, then press 'f' to freeze.

    Used as a diagnostic: with everything frozen the renderer still runs at the
    full 60 Hz, so if the picture still shimmers the cause is in the display
    path (double buffering / DDR contention / HDMI); if it goes rock steady the
    cause is some per-frame content change.

    .\game_freeze.ps1                # scene 7, wait 6 s, freeze, log 10 s
    .\game_freeze.ps1 -Wait 12       # longer before freezing (let the boss come)
    .\game_freeze.ps1 -NoFreeze      # just go to the game and watch
#>
param(
    [int]$Wait = 6,
    [int]$Hold = 10,
    [switch]$NoFreeze,
    [string]$Port = "COM18"
)

$ErrorActionPreference = "Stop"

function Open-Port($p) {
    $sp = New-Object System.IO.Ports.SerialPort $p, 115200, "None", 8, "One"
    $sp.ReadTimeout = 300
    $sp.Open()
    Start-Sleep -Milliseconds 150
    [void]$sp.ReadExisting()
    return $sp
}

Write-Host ("opening {0} ..." -f $Port)
try { $sp = Open-Port $Port }
catch {
    Write-Host ("cannot open {0}: {1}" -f $Port, $_.Exception.Message) -ForegroundColor Red
    exit 1
}

try {
    Write-Host "-> scene 7 (STARFALL)"
    $sp.Write("7")
    Start-Sleep -Milliseconds 200

    Write-Host ("waiting {0} s ..." -f $Wait) -ForegroundColor Cyan
    $deadline = [DateTime]::Now.AddSeconds($Wait)
    while ([DateTime]::Now -lt $deadline) {
        try { $t = $sp.ReadExisting() } catch { $t = "" }
        if ($t) { Write-Host $t -NoNewline }
        Start-Sleep -Milliseconds 60
    }

    if (-not $NoFreeze) {
        Write-Host ""
        Write-Host "-> f  (freeze: simulation stops, rendering keeps running)" -ForegroundColor Yellow
        $sp.Write("f")
    }

    Write-Host ("watching {0} s ..." -f $Hold) -ForegroundColor Cyan
    $deadline = [DateTime]::Now.AddSeconds($Hold)
    while ([DateTime]::Now -lt $deadline) {
        try { $t = $sp.ReadExisting() } catch { $t = "" }
        if ($t) { Write-Host $t -NoNewline }
        Start-Sleep -Milliseconds 60
    }
} finally {
    $sp.Close()
    $sp.Dispose()
    Write-Host ""
    Write-Host "port released" -ForegroundColor Cyan
}
