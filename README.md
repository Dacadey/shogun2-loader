# Shogun 2 Loader

Engine fixes for Total War: SHOGUN 2, as a `version.dll` that the game loads together with its engine. Battles,
replays and campaigns load 2-3 times faster in DirectX 11 mode, the loading screen no longer waits for vsync, and the
game's threads are no longer pinned to fixed cores. No game file is changed: delete `version.dll` and the game is
exactly as before. It does not touch the rules, the AI or the saves.

**Download:** [Nexus Mods](https://www.nexusmods.com/totalwarshogun2/mods/28). How to install it and check that it
works, the settings and the compatibility with other mods are in [package/README.txt](package/README.txt), the readme
that comes with the download ([in Russian](package/README_RU.txt)).

## What it does

- **Textures** (`texture_fast_path`), the main fix. In DirectX 11 mode the engine creates every texture through D3DX,
  which decodes the .dds data and compresses it again; that was most of a battle's load time. When the file already
  holds the format the engine asks for, the DLL creates the texture straight from the file's data. Other textures
  still go through D3DX.
- **Loading screen without vsync** (`loads_without_vsync`). During a load the engine stays in one long frame and
  redraws the loading screen hundreds of times, and with vsync on every redraw waited for the monitor. The DLL
  presents these redraws without the wait; the frames of the game itself keep vsync as set.
- **Pack files through the file cache** (`pack_cache`). The engine opens its .pack files with
  `FILE_FLAG_NO_BUFFERING`, so every load reads the disk again. The DLL has them read through the Windows file cache.
- **Threads** (`threads`, `main_thread`, `workers`, `worker_count`). The engine pins the main thread to core 0 and
  each worker thread to a core of its own, the E-cores of hybrid Intel CPUs included. The DLL leaves them unpinned,
  with the main thread preferring the performance cores.
- **Intro movies** (`skip_intro_movies`, off by default): straight to the main menu.
- **Log and crash dumps.** `shogun2_engine.log` next to `shogun2.exe` has the settings, load times, frame rates and
  the use of the 32-bit address space; when the game crashes, the DLL writes a minidump.

Every fix can be turned off in `shogun2_engine.ini`; see Settings in [package/README.txt](package/README.txt).

## Measured

| Load | Original | With the DLL |
|---|---:|---:|
| Battle replay. Core i9-13900KF, RTX 4090, 4K at 120 Hz | 18.5 s | 4.7 s |
| Sekigahara. Core Ultra 9 185H, RTX 4090 Laptop, 2560x1600 at 240 Hz | 19.0 s | 8.6 s |
| Back from that battle to the menu, same laptop | 17.1 s | 3.3 s |
| Game start to its first frame, intro movies skipped, same laptop | 41.5 s | 20.1 s |

Both PCs: NVMe SSD, Windows 11, vsync on. On the first PC nine historical battles, a campaign and a campaign battle
also loaded 2-3 times faster. Of that, the loading screen without vsync saved 1.4-3.4 s per load at 120 Hz and almost
nothing at 240 Hz; 60 Hz has not been measured.

## How it works

`empire.retail.dll`, the engine, imports `version.dll`, so a DLL of that name in the game folder is loaded together
with the engine, on the main thread, before any engine code runs. It loads the real `version.dll` from the Windows
system folder and forwards every function to it.

The fixes change the engine's own code in memory, in a few places. They are made for one build of the engine:
`empire.retail.dll` 1.1.0 of 12 May 2023, the current Steam version. The DLL checks the engine's time stamp and image
size, and before each change it checks that the engine holds exactly the expected bytes there. With any other build it
changes nothing, only forwards, and says so in its log. It hooks nothing in DirectX or in other programs.

Besides its log, the DLL writes only its settings file (when it is missing), the frame log if `frame_log=1`, and crash
dumps to `%APPDATA%\The Creative Assembly\Shogun2\crash_dumps` (the 10 newest are kept).

[engine/src/engine.cpp](engine/src/engine.cpp) is the whole DLL, with [exports.def](engine/src/exports.def). The
engine's addresses are at its top, and each fix is a numbered section with a comment on what the engine does there
and what the DLL changes.

## Building

Visual Studio with the C++ tools ("Desktop development with C++"); the release was built with Visual Studio 2026. The
DLL is 32-bit, like the game.

```
engine\build.cmd
```

builds `engine\build\version.dll`. The script finds Visual Studio through vswhere, or uses the x86 Developer Command
Prompt it is run from.

## Tests

The tests need the game, the Steam version 1.1.0, and only read its folder. If the game is not in Steam's usual
place, set `SHOGUN2_DIR` to its folder. Build `version.dll` first.

- `engine\test\test.cmd` copies the engine and its DLLs into `engine\test\build\input` and, without starting the
  game, runs the engine's own code through every changed place: the thread setup, the pack files, the intro movies,
  the per-frame call, and the present with and without an overlay that hooks it the way Steam's does. Then another
  build of the engine (left alone), the engine unloaded as at the end of the game, a stack overflow (logged, with a
  minidump that the test deletes again), settings files written the way players write them, and a game folder with
  Cyrillic letters. Under a minute.
- `engine\test\tex_test.cmd` compares the textures of the fast path with those of D3DX, on a sample of the game's own
  textures that `tools\dds_sample.py` picks from its .pack files. Needs Python 3 and the DirectX June 2010 runtime,
  which the game installs. The first run, which picks the sample, takes several minutes.

`tools\probe\topo_probe.cpp` runs the engine's CPU-topology and thread-pool code outside the game and prints which
cores it pins the threads to.

## License

MIT, see [LICENSE](LICENSE). The repository holds no files of the game; of its code there are only the addresses and
the few bytes that the DLL compares before changing them. This project is not affiliated with Creative Assembly or
SEGA.

Written with the help of Claude, an AI assistant made by Anthropic.
