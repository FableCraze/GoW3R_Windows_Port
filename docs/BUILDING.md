# Building on Windows

## Dependencies

- Windows 10/11 x64;
- Python 3.10+;
- CMake 3.24+ and Ninja;
- MinGW-w64 x86-64;
- SDL3, Vulkan SDK 1.3, and `glslangValidator`;
- FFmpeg (`avformat`, `avcodec`, `avutil`, `swscale`, and `swresample`);
- LibAtrac9, obtained through the repository submodule.

Clone the repository with its submodules:

```bat
git clone --recurse-submodules REPOSITORY_URL
cd GoW3R_Windows_Port
```

For an existing clone:

```bat
git submodule update --init --recursive
```

## Development build

When the dependencies are installed in locations detected by the build script,
run:

```bat
build.bat
```

The build uses `RelWithDebInfo`: it generates optimized code while retaining
debug symbols and does not require a separate release package. The main output is
`out\gow3r-port.exe`.

Set custom dependency paths before building when necessary:

```bat
set W64DEVKIT_DIR=D:\path\to\w64devkit
set SDL3_DIR=D:\path\to\SDL3\x86_64-w64-mingw32
set VULKAN_SDK=C:\VulkanSDK\version
build.bat
```

`scripts\check_build_env.py` reports every dependency that could not be found.

## Tests

```bat
build.bat --test
```

Public tests use synthetic fixtures. Tests requiring a game dump remain local.

## Running

```bat
launch_gui.bat
```

or:

```bat
run.bat "D:\path\to\CUSA01623 GoW3Remaster 1.02"
```

A separate Release configuration is not required for debugging.
