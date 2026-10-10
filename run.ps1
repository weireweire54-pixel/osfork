$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

& "$scriptDir\build.ps1"

$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
if (-not (Test-Path $qemu)) {
    throw "QEMU not found at $qemu. Install QEMU and try again."
}

$diskImage = Join-Path $scriptDir 'build\osfork-data.img'
if (-not (Test-Path $diskImage)) {
    $diskStream = [System.IO.File]::Create($diskImage)
    try {
        $diskStream.SetLength(16MB)
    } finally {
        $diskStream.Dispose()
    }
}

Write-Host "Launching osfork in QEMU with persistent virtual disk..."
& $qemu -kernel "$scriptDir\build\kernel.elf" -drive "file=$diskImage,format=raw,if=ide,index=0" -display curses -no-reboot -monitor none
