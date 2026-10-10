# Contributing

Thanks for taking an interest in osfork. This is an experimental educational kernel; small, focused contributions are easiest to review.

## Before opening a pull request

1. Open an issue to discuss larger changes.
2. Keep changes focused and describe how you tested them.
3. Run `build.ps1` from Windows PowerShell and, for runtime changes, try `run.ps1` in QEMU.
4. Do not include generated files from `build/`, disk images, or the QEMU installer.

Good starter tasks include improving shell input, adding filesystem edge-case checks, documenting kernel subsystems, and adding another small built-in game.