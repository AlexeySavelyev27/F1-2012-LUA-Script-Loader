# F1 2012 LUA Script Loader

This project is a Lua plugin loader for the game **F1 2012**.  The code builds a
`dinput8.dll` that is injected into the game to provide an in-game overlay and
Lua scripting API.

## Features

- Loads Lua scripts from the `plugins` folder.
- Displays an ImGui based overlay that shows plugin information.
- Includes a free camera plugin as an example.
- Uses [MinHook](https://github.com/TsudaKageyu/minhook) for API hooking and
  LuaJIT for script execution.

## Repository layout

```
MinHook/        - prebuilt MinHook headers and libraries
Release/        - build artefacts (ignored by git)
imgui/          - ImGui sources used for the overlay
lua/            - LuaJIT sources and libraries
plugins/        - example plugins (.lua + .ini)
main.cpp        - DLL entry point and loader implementation
*.vcxproj       - Visual Studio project files
```

## Building

The project targets Windows and is built with **Visual Studio 2022**.  Open the
`sln` file and build the `dinput8` project in either x86 or x64 configuration.
The resulting `dinput8.dll` is placed in `Release/`.

## Usage

Copy the compiled `dinput8.dll` and the `plugins` directory next to the game
executable (`F1_2012.exe`).  At runtime the loader reads optional
`dinput8_config.ini` settings and executes all Lua plugins found in `plugins`.

## Developing plugins

A plugin consists of an `.ini` file with metadata and a `.lua` script that
defines at least an `OnFrame()` function.  See `plugins/free_cam.lua` and its
accompanying `.ini` for an example.

## License

The code for the loader itself has no explicit license.  The bundled MinHook
library is released under the BSD license (see its header).  Consult the
individual libraries for their terms.
