$ErrorActionPreference = 'Stop'

$llvm = 'C:\Program Files\LLVM\bin'
$build = '.\build'

New-Item -ItemType Directory -Force -Path $build | Out-Null

& "$llvm\clang.exe" --target=i386-elf -ffreestanding -O2 -Wall -Wextra -fno-pic -fno-stack-protector -fno-builtin -fno-omit-frame-pointer -m32 -mno-sse -mno-sse2 -mno-mmx -c .\src\kernel.c -o "$build\kernel.o"
if ($LASTEXITCODE -ne 0) { throw "clang failed compiling kernel.c (exit code $LASTEXITCODE)" }
& "$llvm\clang.exe" --target=i386-elf -ffreestanding -m32 -c .\src\boot.s -o "$build\boot.o"
if ($LASTEXITCODE -ne 0) { throw "clang failed compiling boot.s (exit code $LASTEXITCODE)" }
& "$llvm\clang.exe" --target=i386-elf -ffreestanding -m32 -c .\src\interrupts.s -o "$build\interrupts.o"
if ($LASTEXITCODE -ne 0) { throw "clang failed compiling interrupts.s (exit code $LASTEXITCODE)" }
& "$llvm\ld.lld.exe" -m elf_i386 -T .\src\linker.ld -nostdlib -static -o "$build\kernel.elf" "$build\boot.o" "$build\interrupts.o" "$build\kernel.o"
if ($LASTEXITCODE -ne 0) { throw "ld.lld failed linking kernel.elf (exit code $LASTEXITCODE)" }

Write-Host "Built kernel: $build\kernel.elf"
