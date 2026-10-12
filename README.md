# GoW3R Windows Port

An experimental compatibility layer for running the original x86-64 code of
**God of War III Remastered**, CUSA01623 update 1.02, on Windows.

The game code runs directly on the host CPU. This project provides the SELF/ELF
loader, PS4 ABI, HLE services, and GNM-to-Vulkan graphics translation. It is not
intended to be a general-purpose PlayStation 4 emulator.

> [!IMPORTANT]
> This repository does not contain, download, or decrypt the game, firmware,
> `eboot.bin`, PRX modules, or any other proprietary content. A complete,
> decrypted dump obtained from your own copy is required.

## Compatibility

- God of War III Remastered;
- Title ID `CUSA01623`;
- update `01.02`;
- Windows 10/11 x64;
- x86-64 CPU with AVX2 support;
- GPU and driver with Vulkan 1.3 support;
- 1920×1080 default output and the original 60 Hz behavior.

The project is under active development and does not have a stable release yet.
See the [implementation status](docs/IMPLEMENTATION_STATUS.md) and
[changelog](docs/CHANGELOG.md) for development progress.

## Runtime requirements

- Python 3.10 or newer;
- a decrypted dump of CUSA01623 update 1.02;
- `libc.prx` and `libSceFios2.prx` from the same dump;
- compatible Vulkan and SDL3 runtimes.

## Running

Start the graphical launcher:

```bat
launch_gui.bat
```

Alternatively, provide the game directory directly:

```bat
run.bat "D:\path\to\CUSA01623 GoW3Remaster 1.02"
```

The launcher verifies the title ID, version, and loadable-image hash before
preparing the cache. Generated files are stored in `out`, user data is stored in
`user/CUSA01623`, and the local log is written to `gow3r.log`.

## Building

The development build uses CMake, Ninja, and MinGW-w64:

```bat
build.bat
```

SDL3, the Vulkan SDK, FFmpeg, LibAtrac9, and `glslangValidator` are also required.
See [Building on Windows](docs/BUILDING.md) for complete environment setup
instructions.

## Project structure

- `src/`: loader and Win32/PS4 runtime;
- `gpu/`: Vulkan renderer and shader recompiler derived from shadPS4;
- `scripts/`: preparation, linking, validation, and packaging tools;
- `patches/`: supported optional patches for CUSA01623;
- `tests/`: tests based on free and synthetic fixtures;
- `docs/`: architecture, build, implementation, and provenance documentation.

See the [architecture overview](docs/ARCHITECTURE.md) for more information.

## Supported features

The launcher provides the optional `Skip Intro` and
`Skip Any Video With X Button` patches. Online services, PSN, multiplayer, DLC,
mods, FPS patches, frame generation, and distribution of game assets are outside
the current scope.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request.
Do not attach ROMs, firmware, executables, modules, save files, memory dumps, or
other files derived from the game.

## License and provenance

This project is distributed under **GPL-2.0-or-later**. Its initial architecture
is derived from
[bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port),
and the vendored graphics core is derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4). These references are retained
for attribution and source-code provenance.

See [LICENSE](LICENSE), [Third-party code and provenance](docs/THIRD_PARTY.md),
and [gpu/VENDOR.txt](gpu/VENDOR.txt) for license, version, and backport details.

## Acknowledgements

We gratefully acknowledge the work and ideas of **MrSuS60** ([GitHub](https://github.com/Mrsuss60)), creator of [bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port), and **deadinside28** ([GitHub](https://github.com/deadinside28)), creator of [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc). These projects inspired the direction of GoW3R Windows Port, and this work would not have been possible without their contributions to the community. Thank you for making your work available to others.
