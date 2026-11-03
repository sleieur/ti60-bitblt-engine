<#
    build.ps1 - command-line build of bbShow (same result as the IDE build)

    The Efinity RISC-V IDE ships its own make.exe (busybox) and the
    riscv-none-elf toolchain.  This script finds both, works around the
    "make + SHELL path with spaces" problem by creating a junction without
    spaces, and then invokes exactly the same makefile the IDE uses.

    Usage:  .\build.ps1            (build)
            .\build.ps1 -Clean
#>
param(
    [switch]$Clean,
    [string]$Make,
    [string]$ToolBin
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-First($paths, $leaf) {
    foreach ($p in $paths) {
        if ($p -and (Test-Path -LiteralPath (Join-Path $p $leaf))) { return (Join-Path $p $leaf) }
    }
    return $null
}

# ---------------------------------------------------------------- toolchain
$ideRoots = @(
    "D:\ws\Efinity RISC-V",
    "D:\易灵思\Efinity RISC-V",
    "D:\efinity_riscv_ide\Efinity-RISCV-IDE\..",
    "C:\efinity_riscv_ide\Efinity-RISCV-IDE\..",
    "$env:USERPROFILE\efinity_riscv_ide\Efinity-RISCV-IDE\.."
)

if (-not $ToolBin) {
    foreach ($r in $ideRoots) {
        $c = Join-Path $r "toolchain\bin\riscv-none-elf-gcc.exe"
        if (Test-Path -LiteralPath $c) { $ToolBin = Join-Path $r "toolchain\bin"; break }
    }
}
if (-not $ToolBin) {
    $g = Get-ChildItem -Path "D:\","C:\" -Filter "riscv-none-elf-gcc.exe" -Recurse -Depth 5 -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($g) { $ToolBin = $g.DirectoryName }
}
if (-not $ToolBin) { throw "riscv-none-elf-gcc.exe not found. Pass -ToolBin <dir>." }

# --------------------------------------------------------------------- make
if (-not $Make) {
    foreach ($r in $ideRoots) {
        $c = Join-Path $r "build_tools\bin\make.exe"
        if (Test-Path -LiteralPath $c) { $Make = $c; break }
    }
}
if (-not $Make) {
    $m = Get-ChildItem -Path "D:\","C:\" -Filter "make.exe" -Recurse -Depth 5 -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) { $Make = $m.FullName }
}
if (-not $Make) { throw "make.exe not found. Pass -Make <path>." }

$makeDir = Split-Path -Parent $Make

# The busybox shell used by this makefile cannot cope with spaces in a path,
# so expose both directories through junctions without spaces.
function Get-SpaceFreePath($dir, $name) {
    if ($dir -notmatch ' ') { return $dir }
    $link = Join-Path $env:TEMP $name
    if (-not (Test-Path -LiteralPath $link)) {
        New-Item -ItemType Junction -Path $link -Target $dir | Out-Null
    }
    return $link
}

$shellDir = Get-SpaceFreePath $makeDir  "bbshow_make"

# gcc locates cc1 relative to <prefix>/bin, so the junction must sit one level
# above bin, not on bin itself.
$tcRoot = Split-Path -Parent $ToolBin
$tcLink = Get-SpaceFreePath $tcRoot "bbshow_tc"
$toolDir = Join-Path $tcLink "bin"

$shell = (Join-Path $shellDir "sh.exe").Replace('\', '/')

$env:PATH = "$shellDir;$toolDir;" + $env:PATH

Push-Location $here
try {
    if ($Clean) { & (Join-Path $shellDir "make.exe") "SHELL=$shell" clean }
    & (Join-Path $shellDir "make.exe") `
        "SHELL=$shell" `
        "RISCV_BIN=$($toolDir.Replace('\','/'))/riscv-none-elf-" `
        "BSP=efinix/EfxSapphireSoc"
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
finally { Pop-Location }

$bin = Join-Path $here "build\bbShow.bin"
if (Test-Path -LiteralPath $bin) {
    Write-Host ""
    Write-Host ("bbShow.bin  {0} bytes" -f (Get-Item -LiteralPath $bin).Length)
}
