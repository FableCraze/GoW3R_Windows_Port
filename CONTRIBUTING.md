# Contributing

Thank you for your interest in the GoW3R Windows Port.

## Before submitting a change

1. Create a short, focused branch.
2. Do not include game files or derived content. See `docs/GAME_FILES.md`.
3. Preserve the GPL-2.0-or-later license and provenance in `gpu/VENDOR.txt`.
4. Add or update tests for loader, runtime, or GPU changes.
5. Run the relevant tests and describe the hardware, driver, and scenario used.

## Bug reports

Include the Windows version, CPU, GPU, driver, tested commit, relevant excerpts
from `gow3r.log`, and reproduction steps. Remove personal paths. Do not attach
ROMs, PRX, WAD, XVAG, BSF, save files, or game memory dumps.

For graphics issues, identify the exact point in the campaign and say whether the
problem appears on the first frame or only after a cutscene transition.
