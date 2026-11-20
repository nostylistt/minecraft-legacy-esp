# Minecraft Legacy ESP

Educational research project: an OpenGL-based visual overlay for Minecraft 1.8.9
(Lunar / vanilla 1.8.9 / third-party 1.8.9 clients).

The DLL hooks a handful of legacy `opengl32.dll` calls and reads the live HotSpot
JVM heap to reconstruct where other players are on screen, then draws an overlay
in the same GL context. No OS-level overlay windows, no DirectX hooks.

Feature set is intentionally small:

- **2D box** — screen-space rectangle around each rendered player
- **Tracers** — line from a screen anchor to each player
- **Nametags** — real nicknames above players (read from the JVM, matched to models)
- **Distance** — meters under each nametag
- **Glow** — soft outline silhouette through walls (FBO + shader)
- **Wallhack** — depth bias to render player models over terrain
- **Auto clicker** — timed left-click while a bind (or physical LMB) is held
- **Aimbot** — soft aim at the nearest player's chest while a bind is held

No license server, no auth, no telemetry, no "kill switch", no payload download.
Local-only, single file.

## Layout

```
dll/        — the injectable DLL (OpenGL overlay + JVM memory reader + ImGui menu)
injector/   — minimal LoadLibrary injector (single-file console exe)
```

## Prebuilt binaries

Ready-to-run x64 binaries live in `release/`:

- `release/legacy_esp.dll`
- `release/legacy_esp_injector.exe`

Both are built against the sources in this repo with the commands in the next
section. If you'd rather not trust the prebuilt files, just build your own.

## Build

Requires Visual Studio 2022 Build Tools (`v143` toolset, Windows 10 SDK).
From a developer command prompt:

```
msbuild dll\legacy_esp.vcxproj /p:Configuration=Release /p:Platform=x64 /p:TargetName=legacy_esp
msbuild injector\legacy_esp_injector.vcxproj /p:Configuration=Release /p:Platform=x64
```

Outputs:

- `dll\x64\Release\legacy_esp.dll`
- `injector\out\legacy_esp_injector.exe`

## Use

1. Place `legacy_esp.dll` and `legacy_esp_injector.exe` in the same folder.
2. Run the injector. You can start it either before or after Minecraft:
   - If Minecraft is already running, it attaches to the newest `javaw.exe`.
   - Otherwise it waits for one to start.
3. In game, press **INSERT** to open the menu.

Optional second argument to the injector forces hook mode:

```
legacy_esp_injector.exe legacy_esp.dll direct   # Lunar-style: direct MinHook on opengl32 exports
legacy_esp_injector.exe legacy_esp.dll resolve  # resolve-hook via GetProcAddressForCaller
```

Default is `auto`: direct if `opengl32.dll` is already loaded in the target
(i.e. Minecraft is running), resolve otherwise.

## How it works, briefly

- **Overlay** is drawn by a hook on `wglSwapBuffers`. Hooks on `glScalef`,
  `glRotatef`, `glTranslatef`, `glEnable`, `glDisable`, `glCallList`, `glOrtho`
  capture entity geometry, camera orientation, and perform the ESP draws.
- **Player data** (nicknames, world positions) comes from reading the running
  HotSpot JVM heap directly. A background discovery thread auto-detects the
  Minecraft client/entity class layout by scanning for stable landmarks, so
  this works across Lunar 1.8.9, Java 8 vanilla 1.8.9, and Java 9+ clients
  (compact string support).
- **Menu** uses Dear ImGui with an OpenGL2 or OpenGL3 backend, selected at
  compile time (`USE_GL3_BACKEND`).

## Compatibility

- Minecraft **1.8.9** only. Protocol/class layouts for other versions are different.
- 64-bit HotSpot JVM (OpenJDK, Lunar bundled JRE, etc.).
- Windows x64. The DLL and injector are x64; you need a 64-bit Java process to inject into.

## Scope

This is a learning / reverse-engineering project. It is not a game hack product,
has no anti-cheat bypass, and does not interact with any network — all behavior
is local to the one JVM it runs in. Use it on offline singleplayer worlds or
private servers you own. Respect the terms of service of any server you connect to.

## License

MIT. See `LICENSE`.
