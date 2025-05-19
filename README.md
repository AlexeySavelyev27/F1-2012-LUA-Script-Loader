# F1hook

## Prerequisites
- **Visual Studio 2022 or later** with the C++ toolset installed.
- **LuaJIT 2.1** (or Lua 5.1) headers and libraries. Prebuilt libraries are available under `lua/build`.

## Building `dinput8.dll`
1. Open `dinput8.sln` in Visual Studio.
2. Select either the `x86` or `x64` configuration.
3. Build the solution. The resulting DLL will be `dinput8.dll`.

Copy this DLL alongside the F1 2012 executable so it is loaded on startup.

## Configuration
The loader reads `dinput8_config.ini` located next to the DLL. Important settings include:

```
hook.toggleKey=F9   ; show overlay / switch plugin
hook.closeKey=F10   ; hide overlay
hook.reloadKey=F8   ; reload current plugin
hook.pluginFolder=plugins
```

Other options allow tweaking overlay color, position and logging.

## Plugin folder layout
Plugins reside inside the folder specified by `hook.pluginFolder` (defaults to `plugins`). Each plugin uses two files with the same base name:

```
myplugin.ini   ; metadata such as meta.name, meta.version, meta.author
myplugin.lua   ; Lua script executed by the loader
```

The loader watches this directory for changes and automatically loads new or updated plugins.

## Using the loader
- **F9** – show the overlay. Press again to cycle between detected plugins.
- **F10** – hide the overlay.
- **F8** – reload the currently selected plugin.

Place your plugins and configuration file next to `dinput8.dll`, then launch the game to activate the loader.
