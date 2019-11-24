# DuckStation - PlayStation (PSX) Emulator
![Main Window Screenshot](https://raw.githubusercontent.com/stenzek/duckstation/md-images/main.png)

## Features
 - CPU Recompiler/JIT (x86_64 work-in-progress, AArch64 planned)
 - Hardware (D3D11 and OpenGL) and software rendering
 - Upscaling and true colour (24-bit) in hardware renderers
 - "Fast boot" for skipping BIOS splash/intro
 - Save state support
 - Windows and Linux support - macOS may work, but not supported by author

## Building
Clone the respository with submodules (`git clone --recursive` or `git clone` and `git submodule update --init`).

### Windows
Requirements:
 - Visual Studio 2019

1. Open the Visual Studio solution `duckstation.sln` in the root, or "Open Folder" for cmake build.
2. Build, binaries are located in `bin/x64`.
3. Copy the DLL files from `dep/msvc/bin64` to the binary directory.
4. Run `duckstation-x64-Release.exe` or whichever config you used.

### Linux
Requirements:
 - CMake
 - SDL2

1. Create a build directory, either in-tree or elsewhere.
2. Run cmake to configure the build system. Assuming a build subdirectory of `build-release`, `cd build-release && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ..`.
3. Compile the source code. For the example above, run `ninja`.
4. Run the binary, located in the build directory under `src/duckstation/duckstation`.

## Running
1. Configure the BIOS path in the settings.
2. Open a disc image file, enjoy.

## Default keyboard bindings
Keyboard bindings are currently not customizable. For reference:
 - **D-Pad:** W/A/S/D or Up/Left/Down/Right
 - **Triangle/Square/Circle/Cross:** I/J/L/K or Numpad8/Numpad4/Numpad6/Numpad2
 - **L1/R1:** Q/E
 - **L2/L2:** 1/3
 - **Start:** Enter
 - **Select:** Backspace

Gamepads are automatically detected and supported. Tested with an Xbox One controller.
To access the menus with the controller, press the right stick down and use the D-Pad to navigate, A to select.

## Useful hotkeys
 - **F1-F8:** Quick load/save (hold shift to save)
 - **F11:** Toggle fullscreen
 - **Tab:** Temporarily disable speed limiter
 - **Pause/Break:** Pause/resume emulation
 - **Space:** Frame step
 - **End:** Toggle software renderering
 - **Page Up/Down:** Increase/decrease resolution scale in hardware renderers

## Tests
 - Passes amidog's CPU and GTE tests, partial passing of CPX tests

## Screenshots
![Final Fantasy 7](https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff7.jpg)
![Final Fantasy 8](https://raw.githubusercontent.com/stenzek/duckstation/md-images/ff8.jpg)
![Spyro 2](https://raw.githubusercontent.com/stenzek/duckstation/md-images/spyro.jpg)

## Disclaimers

Icon by icons8: https://icons8.com/icon/74847/platforms.undefined.short-title

"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.