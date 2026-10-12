# Implementation status

## Working in local testing

- CUSA01623 01.02 and loadable-image hash validation;
- `GOW3IMG1` image, relocation, per-module TLS, and `libc.prx`/`libSceFios2.prx` linking;
- static coverage of 253/253 host contracts after 134 native bindings;
- entry point, boot sequence, logos, cinematics, menu, and gameplay entry;
- 60 FPS video on the development hardware, audio, and controller input;
- corrected distant backgrounds, Kratos's weapons, opening enemies, and lighting in tested scenarios;
- rendered vegetation, particles, and fire, with density differences remaining in some areas.

## Known issues

- some Gaia textures and vegetation during the opening sequence still differ from the expected result;
- Gaia's face may display incorrect mip or tiling behavior during gameplay;
- the AMD/NVIDIA validation matrix is not yet complete;
- save/reload and the continuous 30-minute session still require final v0.1 validation.

## v0.1 acceptance criteria

1. Complete 30 minutes of the opening sequence without missing imports, heap
   corruption, deadlocks, device loss, or continuous audio loss.
2. Create a checkpoint, exit, restart, and restore the save correctly.
3. Repeat the scenario on at least one AMD GPU and one NVIDIA GPU.
4. Validate cinematics, XVAG/ATRAC9 playback, and A/V synchronization.
5. Do not distribute any game file or derivative.
