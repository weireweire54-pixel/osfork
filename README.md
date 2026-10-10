# osfork

A minimal 32-bit x86 bare-metal operating system built as a multiboot kernel. The current project includes a working boot entry, VGA text-mode terminal, keyboard input, an in-memory filesystem, interrupt and timer handling, a task scheduler, a shell, and protected-mode task transition groundwork.

## Features

- multiboot-compatible 32-bit kernel entry
- VGA text-mode display and terminal output
- PS/2-style keyboard input and line editing
- simple in-memory filesystem with directories and files
- filesystem snapshots saved to a QEMU IDE disk image
- interrupt and timer handling
- task scheduler and basic process lifecycle
- interactive shell with commands such as help, ls, cd, cat, and browse

## Build

```powershell
cd C:\Users\7782157630\osfork
.\build.ps1
```

## Run

```powershell
cd C:\Users\7782157630\osfork
& "C:\Program Files\qemu\qemu-system-i386.exe" -kernel .\build\kernel.elf -display curses -no-reboot
```

You can also run the helper script:

```powershell
.\run.ps1
```

The shell is designed to stay active while you type and supports a local text browser via the `browse` command.

## Persistent filesystem

`run.ps1` creates and attaches `build\osfork-data.img`, a dedicated 16 MiB virtual IDE disk. Files and directories created with `touch`, `mkdir`, and `rm` are saved automatically and restored on the next launch. Keep this image to retain data; deleting it resets the filesystem to its default contents. The persistence layer is a small snapshot format for this project, not a general-purpose disk filesystem.
