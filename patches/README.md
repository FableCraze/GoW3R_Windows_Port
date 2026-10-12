# God of War III Remastered patches

`God_of_War_III_Remastered.xml` was copied byte-for-byte from the local
`cuesta4-shadPS4-eltutz` checkout of https://github.com/cuesta4/shadPS4,
commit `eb325fbd64c1696389b60813df125f517736573f`, on 2026-10-12.
Original author names, notes, addresses and values are preserved.

The launcher exposes the nine eboot patches for version 01.02. It selects
`Bug Fix - Texture Corruption Fix` by default instead of blindly activating the
XML's `isEnabled` flags: this local XML marks both that fix and 720p enabled,
although its notes prohibit using them together. A single selector enforces
mutual exclusion of the texture fix and the five resolution patches.

| Selection | Game output | Required additional DMEM | Runtime total DMEM |
| --- | --- | --- | --- |
| Texture Corruption Fix | Original (1920×1080) | 2048 MiB | 7104 MiB |
| 480p | 854×480 | 2048 MiB | 7104 MiB |
| 720p | 1280×720 | 2048 MiB | 7104 MiB |
| 1440p | 2560×1440 | 3072 MiB | 8128 MiB |
| 1800p | 3200×1800 | 4608 MiB | 9664 MiB |
| 4K | 3840×2160 | 6144 MiB | 11200 MiB |

The fork's DMEM setting adds memory to the PS4's retail 5056 MiB pool; this port's
`GOW3R_DMEM_MB` controls the total. The 120 FPS patch also sets
`GOW3R_VBLANK_HZ=120` and `GOW3R_FPS_LIMIT=120`.

For command-line use, set `GOW3R_PATCHES` to XML patch names separated by `;`.
Set it to `none` to disable the graphics patches. The existing
`GOW3R_PATCH_SKIP_INTRO` and `GOW3R_PATCH_SKIP_VIDEO` switches still work.
`run.bat` compiles the selection every launch and applies its runtime requirements.

XML addresses are based at `0x400000`. The texture fix at `0x007cd918` writes
`00000090` at image offset `0x003cd918` after relocation. The previous automatic
linker write at image offset `0x007cd918` was a different location in BSS and has
been removed. The cache schema changed so cached linked images are rebuilt once.
Neither the original eboot nor other game files are modified.
