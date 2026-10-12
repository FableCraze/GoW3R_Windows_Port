# Architecture

## Startup flow

1. `launcher.py` or `run.bat` receives the user's dump directory.
2. `game_check.py` validates CUSA01623, update 1.02, and the known loadable-image hash.
3. `prepare.py` analyzes the SELF/ELF, relocations, TLS, and imports, then produces a `GOW3IMG1` image.
4. `link_modules.py` links the user-provided `libc.prx` and `libSceFios2.prx` modules.
5. `import_coverage.py` requires complete coverage of all host contracts.
6. `gow3r-port.exe` maps the image, mounts the dump as `/app0`, and transfers control to the entry point.

## Runtime

The runtime implements the SysV ABI used by the PS4 executable on Win64 and
provides virtual memory, TLS, threads, synchronization, timing, events, files,
Fios2, controller input, audio, save data, dialogs, and offline services. Unknown
imports are fatal errors; they never return silent success.

## GPU

The renderer is derived from the shadPS4 Liverpool/Vulkan core. GNM commands are
interpreted on the host, GCN shaders are recompiled, and resources are presented
through Vulkan. GoW3R-specific tiling, aliasing, upload, and readback adaptations
are documented in `gpu/VENDOR.txt`.

## Generated data

`out/` contains generated images, manifests, cache files, and local binaries.
`user/CUSA01623/` contains local save data and trophies. Neither directory should
be committed to version control.
