<#
    serial_log.ps1 - read the board's UART without installing anything.

    The board's FT4232 channel A shows up as a normal COM port.  This uses the
    .NET SerialPort class that ships with Windows, so there is nothing to
    install, and it works as a plain log viewer while you record.

    Usage:
        .\serial_log.ps1                       # COM16, until Ctrl-C
        .\serial_log.ps1 -Seconds 30           # stop after 30 s
        .\serial_log.ps1 -Port COM18
        .\serial_log.ps1 -Send "1"             # also send a scene key first
        .\serial_log.ps1 -Send " " -Seconds 5  # send SPACE, watch 5 s

    Notes:
      * 115200 8N1, no flow control - what bbShow prints.
      * Only one program can own a COM port.  Close PuTTY / the IDE terminal
        before running this.
      * Ctrl-C stops it; the port is released on the way out.
#>
param(
    [string]$Port = "",          # empty = probe every FTDI port and use the one that talks
    [int]$Baud = 115200,
    [int]$Seconds = 0,
    [string]$Send,
    [string]$LogFile
)

$ErrorActionPreference = "Stop"

# The board's UART is NOT on channel A as one would expect - on this design it
# lands on channel C (COM18 on the machine it was developed on).  Enumerating
# and probing is the only reliable way, and it survives a change of COM number
# when the cable is moved to another USB port.
function Get-FtdiPorts {
    # Only the FT4232 channels are worth probing; the machine has a pile of
    # other virtual COM ports that would just add delay.
    $names = @()
    try {
        Get-PnpDevice -PresentOnly -Class Ports -ErrorAction Stop |
            Where-Object { $_.InstanceId -like "*FTDIBUS*" -and $_.Status -eq "OK" } |
            ForEach-Object {
                if ($_.FriendlyName -match "\((COM\d+)\)") { $names += $matches[1] }
            }
    } catch { }
    if ($names.Count -eq 0) {
        $names = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    }

    # Remember the port that worked last time and try it first - probing all
    # four FTDI channels costs several seconds otherwise.
    $hintFile = Join-Path $env:TEMP "bbshow_uart.txt"
    if (Test-Path -LiteralPath $hintFile) {
        $hint = (Get-Content -LiteralPath $hintFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($hint -and ($names -contains $hint)) {
            $names = @($hint) + ($names | Where-Object { $_ -ne $hint })
        }
    }
    return $names
}

function Save-PortHint($port) {
    if (-not $port) { return }
    try {
        Set-Content -LiteralPath (Join-Path $env:TEMP "bbshow_uart.txt") -Value $port -Encoding ASCII
    } catch { }
}

function Find-TalkingPort($baud) {
    # bbShow prints one line per second, so each port has to be listened to for
    # longer than that - a 120 ms sample sees nothing even on the right port.
    $listen = 1300
    foreach ($p in Get-FtdiPorts) {
        $sp = $null
        try {
            $sp = New-Object System.IO.Ports.SerialPort $p, $baud, "None", 8, "One"
            $sp.ReadTimeout = 200
            $sp.Open()
            $deadline = [DateTime]::Now.AddMilliseconds($listen)
            $got = ""
            while ([DateTime]::Now -lt $deadline) {
                try { $got += $sp.ReadExisting() } catch { }
                if ($got.Length -gt 0) { break }
                Start-Sleep -Milliseconds 50
            }
            if ($got.Length -gt 0) {
                Write-Host ("  {0}: talking -> using it" -f $p) -ForegroundColor Green
                return $p
            }
            Write-Host ("  {0}: silent" -f $p) -ForegroundColor DarkGray
        } catch {
            Write-Host ("  {0}: busy (another terminal has it?)" -f $p) -ForegroundColor DarkGray
        } finally {
            if ($sp) { try { $sp.Close(); $sp.Dispose() } catch {} }
        }
    }
    return ""
}

if (-not $Port) {
    Write-Host "probing serial ports ..."
    $Port = Find-TalkingPort $Baud
    if (-not $Port) {
        Write-Host ""
        Write-Host "no board talking.  is bbShow running?  is the bitstream loaded?" -ForegroundColor Yellow
        Write-Host "ports seen: " -NoNewline
        ([System.IO.Ports.SerialPort]::GetPortNames() -join ", ")
        Write-Host ""
        exit 1
    }
    Save-PortHint $Port
    Write-Host ("using {0}" -f $Port) -ForegroundColor Cyan
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
$sp.ReadTimeout = 500
$sp.WriteTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
} catch {
    Write-Host ("cannot open {0}: {1}" -f $Port, $_.Exception.Message) -ForegroundColor Red
    Write-Host "is PuTTY / another terminal holding it?  available ports:"
    [System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { "  " + $_ }
    exit 1
}

Write-Host ("{0} @ {1} 8N1   (Ctrl-C to stop)" -f $Port, $Baud) -ForegroundColor Cyan
if ($Seconds -gt 0) { Write-Host ("will stop after {0} s" -f $Seconds) }

if ($Send) {
    Start-Sleep -Milliseconds 300
    $sp.Write($Send)
    Write-Host ("sent: '{0}'" -f $Send) -ForegroundColor Yellow
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$buf = New-Object System.Text.StringBuilder

Write-Host ""
Write-Host "CLICK THIS WINDOW FIRST, then the keyboard controls the board." -ForegroundColor Yellow
Write-Host "  1-9 = jump to scene    SPACE = next scene    ESC = quit" -ForegroundColor Yellow
Write-Host ""

try {
    while ($true) {
        if ($Seconds -gt 0 -and $sw.Elapsed.TotalSeconds -ge $Seconds) { break }

        # forward keystrokes to the board: only works while this window has focus
        try {
            if ([Console]::KeyAvailable) {
                $k = [Console]::ReadKey($true)
                if ($k.Key -eq "Enter") {
                    $sp.Write("`r")
                    Write-Host "[tx Enter]" -ForegroundColor DarkYellow
                } elseif ($k.Key -eq "Escape") {
                    break
                } elseif ($k.KeyChar -and [int]$k.KeyChar -ge 32) {
                    $sp.Write([string]$k.KeyChar)
                    Write-Host ("[tx {0}]" -f $k.KeyChar) -ForegroundColor DarkYellow
                }
            }
        } catch { }

        try {
            $chunk = $sp.ReadExisting()
        } catch {
            $chunk = ""
        }
        if ($chunk) {
            Write-Host $chunk -NoNewline
            if ($LogFile) { [void]$buf.Append($chunk) }
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
} finally {
    $sp.Close()
    $sp.Dispose()
    if ($LogFile -and $buf.Length -gt 0) {
        [IO.File]::WriteAllText($LogFile, $buf.ToString())
        Write-Host ""
        Write-Host ("log saved to {0}" -f $LogFile) -ForegroundColor Green
    }
    Write-Host ""
    Write-Host "port released" -ForegroundColor Cyan
}
