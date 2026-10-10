param(
    [ValidateSet('gtk', 'sdl', 'curses')]
    [string]$Display = 'gtk'
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

& "$scriptDir\build.ps1"

$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
if (-not (Test-Path $qemu)) {
    $qemuCommand = Get-Command 'qemu-system-i386.exe' -ErrorAction SilentlyContinue
    if ($qemuCommand) {
        $qemu = $qemuCommand.Source
    } else {
        throw 'QEMU was not found. Install it or add qemu-system-i386.exe to PATH.'
    }
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

Write-Host "Launching osfork in QEMU ($Display display) with persistent virtual disk..."
& $qemu -kernel "$scriptDir\build\kernel.elf" -drive "file=$diskImage,format=raw,if=ide,index=0" -display $Display -no-reboot -monitor none
