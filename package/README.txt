Shogun 2 Loader 0.5.2 - engine fixes for Total War: SHOGUN 2
============================================================

A small DLL that the game loads together with its engine and that fixes a few things in it
while it runs. No game file is changed: delete the DLL and the game is exactly as before.
It does not touch the rules, the AI or the saves.

Which download do I need?
-------------------------
The mod comes as two downloads with the same DLL in them. Install one of them, not both.
- Shogun2Loader-0.5.2.zip, the DLL as version.dll: for most players. Nothing else is needed.
- Shogun2Loader-0.5.2-ASI.zip, the DLL as an ASI plugin (plugins\shogun2_engine.asi): only if
  you use Ultimate ASI Loader, for example for SharpFonts or other .asi mods, or if another mod
  already has its own version.dll in the game folder. Ultimate ASI Loader is not included; get
  it from https://github.com/ThirteenAG/Ultimate-ASI-Loader
In the game the two work the same.

What it does
------------
- Battles, replays and campaigns load 2-3 times faster. This is the main fix: in DirectX 11 mode
  the game decodes every texture and compresses it again (D3DX), which was most of a battle's
  load time; the DLL creates the textures straight from the game's files instead.
- The game starts faster, and going back from a battle to the menu is much quicker (5 times on
  the laptop below).
- With vsync on, the loading screen no longer waits for the monitor on every redraw. How much
  that saves depends on the monitor: 1.3-3.4 s per load at 120 Hz, almost nothing at 240 Hz
  (60 Hz has not been measured). The game itself keeps vsync as set.
- The .pack files are read through the Windows file cache, so a second load of the same data
  comes from memory. On a fast NVMe drive this changes little.
- The main thread and the worker threads are no longer pinned to fixed cores (the game pins the
  main thread to core 0 and each worker thread to a core of its own, the slow E-cores of new
  Intel CPUs included). In our tests the frame rate in battle was no lower; on the laptop's
  hybrid CPU it was 5-10% higher.
- Optional: skips the intro movies (skip_intro_movies=1 in the settings).
- Writes a log, shogun2_engine.log, with the load times and frame rates, and when the game
  crashes it tries to write a crash dump.

Measured on two PCs:
- Core i9-13900KF, RTX 4090, NVMe SSD, 4K at 120 Hz, Windows 11, vsync on: a battle replay
  loaded in 18.5 s before and 4.7 s with the DLL; nine historical battles, a campaign and a
  campaign battle loaded 2-3 times faster.
- Core Ultra 9 185H, RTX 4090 Laptop, NVMe SSD, 2560x1600 at 240 Hz, Windows 11, vsync on: the
  Sekigahara battle loaded in 19.0 s before and 8.6 s with the DLL (17.7 s and 7.8 s in full
  screen); going back from the battle to the menu took 17.1 s and 3.3 s; the game started (to
  its first frame, intro movies skipped both times) in 41.5 s and 20.1 s.

Requirements
------------
- Total War: SHOGUN 2 from Steam, the current version (1.1.0). With any other version of the
  game the DLL changes nothing and says so in its log.
- DirectX 11 mode (the game's default on DirectX 11 graphics cards). In DirectX 9 mode the game
  runs with the DLL too, but the texture and vsync fixes do not apply.
- Windows 10 or 11. Nothing else needs to be installed (for the ASI download: Ultimate ASI
  Loader).

Install
-------
1. Close the game.
2. Open the game's folder: in Steam, Library -> right-click Total War: SHOGUN 2 -> Manage ->
   Browse local files. It is the folder with shogun2.exe and empire.retail.dll, usually
   C:\Program Files (x86)\Steam\steamapps\common\Total War SHOGUN 2
3. Open the "Copy to game folder" folder of the download and copy what is in it into the game's
   folder: its contents, not the folder itself.
   - version.dll download: version.dll and shogun2_engine.ini, so that version.dll lies right
     next to shogun2.exe. If the game folder already has a version.dll from another mod, stop
     and take the ASI download instead (see Compatibility).
   - ASI download: the plugins folder with shogun2_engine.asi in it, and shogun2_engine.ini.
     Ultimate ASI Loader has to be in the game folder already; we tested it installed as
     winmm.dll (version 10.0.0).
4. Start the game from Steam as usual.

Check that it works
-------------------
1. Wait for the main menu. The game folder now has shogun2_engine.log (it is written anew at
   every start). If it has not, the DLL was most likely not loaded (or it could not write in the
   game folder): check that version.dll lies right next to shogun2.exe and not in a folder of
   its own, or, with the ASI download, that Ultimate ASI Loader is installed and
   shogun2_engine.asi is in the plugins folder. Open the log in Notepad. Near the top there
   should be these lines (the times and numbers will differ):
     Shogun 2 engine fixes 0.5.2 (version.dll) loaded into C:\...\Total War SHOGUN 2
     settings: threads=free main_thread=pcores workers=any worker_count=0 pack_cache=1 ...
     threads: pool and main thread will be left unpinned
     pack_cache: on (no FILE_FLAG_NO_BUFFERING)
     frame statistics: on
     textures: fast path on (files that already hold the asked-for format skip D3DX)
     loads_without_vsync: on (loading screens are presented without vsync)
     first frame: 15.5 s after the process started
   With the ASI download the first line names shogun2_engine.asi instead, and the next one says
   that it was loaded before empire.retail.dll and came in at the engine's entry point.
   If there is a line "empire.retail.dll is build ...: nothing is changed, version.dll only
   forwards", this version of the game is not supported, and the game runs as without the mod.
   Lines starting with "ini:" right under "settings:" mean that something in shogun2_engine.ini
   is written in a way the DLL does not understand; each line says which value is used instead
   (see Settings).
2. Load a battle: Single Player -> Historical Battles -> pick a battle -> Start Battle. When it
   has loaded (the Start Battle gong shows), the log has two new lines like these:
     textures since the last load: 1336 created in 513 ms (780 MB of files)
     long frame: 4774 ms; loading screen drawn 228 times, 228 without vsync
   "long frame" is the load time. The textures should take well under a second in total.
   With vsync off in the game's options the last number is 0: there was no wait to remove.
3. Play or watch for a minute: "frames: ..." lines give the frame rate every 30 s. With vsync
   on it is never above the monitor's refresh rate.
4. Leave the battle (Esc, then quit it through the menu) and quit the game from the main menu.
   The last line of the log is: unloaded (the process ends)
5. No crash: no line of the log contains "CRASH (" (in Notepad's Find, tick "Match case": the
   lines "crash_dumps=1" and "crash dumps: on" are settings, not crashes), and the folder
   %APPDATA%\The Creative Assembly\Shogun2\crash_dumps does not exist or is empty.

Compare with the original game (optional)
-----------------------------------------
In shogun2_engine.ini set
  threads=engine
  pack_cache=0
  texture_fast_path=0
  loads_without_vsync=0
start the game, load the same battle and note the "long frame" and the texture time. With these
values the DLL only measures and the game behaves as without it. Then set them back to
free / 1 / 1 / 1. The first load after starting the game is slower than later ones (the files
are not in memory yet), so compare the first load with the first load.

Settings (shogun2_engine.ini, section [engine])
-----------------------------------------------
threads=free            free: Windows places the threads on any core. engine: as the game does.
main_thread=pcores      with threads=free: the main thread prefers the performance cores of
                        hybrid Intel CPUs; any: Windows decides.
workers=any             with threads=free: which cores the worker threads prefer: any, pcores
                        or ecores.
worker_count=0          worker threads, main thread included, from 0 to 64; 0 keeps the game's
                        choice.
pack_cache=1            1: .pack files through the Windows file cache. 0: as the game does.
texture_fast_path=1     1: textures straight from the files. 0: all through D3DX, as the game.
loads_without_vsync=1   1: the loading screen does not wait for vsync (1.3-3.4 s per load at
                        120 Hz). 0: as the game does.
skip_intro_movies=0     1: straight to the main menu, without the intro movies.
frame_log=0             1: also writes shogun2_frames.csv (the time of every frame), for
                        diagnostics: about 15 MB an hour at 240 fps; written anew at every start.
memory_log=1            1: logs the game's use of its 4 GB of addresses every 30 s.
crash_dumps=1           1: writes a crash dump to %APPDATA%\The Creative Assembly\Shogun2\crash_dumps.
The on/off settings take 1 or 0; yes/no, true/false and on/off work too, in any case. A comment
may follow a value after ";". A setting left out of the file keeps its default. A value the DLL
does not understand keeps the default as well, and the log then has an "ini:" line that says so;
so does a setting name it does not know (a typo) and a file without the [engine] line. A setting
written twice counts with its first value (as Windows reads such files), and the log says so too.
If the file is missing, the DLL writes it with these values at the next start.

Compatibility
-------------
- Mods from the Steam Workshop or the launcher's mod manager (.pack files: units, balance, maps,
  HD textures, camera) work together with it. The DLL does not change game data; mod packs are
  read through the same file cache and their textures through the same fast path. Tested with
  12 turns per Year, 2025 Bug Fix Bundle, Shogun 2 HD and Extended Camera Minimod.
- Saves are not touched: a campaign started with the DLL loads without it, and the other way
  round.
- Other mods that are DLLs in the game folder (dinput8.dll, d3d11.dll, dxgi.dll): no conflict is
  known; tested with two such mods for a 4K interface. ReShade has not been tested.
- Another mod that also brings a version.dll (some plugin loaders use that name): only one file
  of that name fits in the game folder, so take the ASI download, with Ultimate ASI Loader.
- Ultimate ASI Loader and .asi mods: take the ASI download. Tested with Ultimate ASI Loader
  10.0.0 as winmm.dll and the game started from Steam; SharpFonts has not been tested. Install
  only one of the two downloads: with both, the DLL is loaded twice, and the second copy finds
  the fixes already made and leaves them.
- Another version of the game (another build of empire.retail.dll): the DLL notices it, changes
  nothing and says so in its log; the game runs as without it. If another mod has edited
  empire.retail.dll but left its build stamp and size as they were, the DLL makes only the
  changes whose code is still exactly as it expects, and logs the others as left alone.
- Overlays and recording: the Steam overlay and NVIDIA's capture are tested, switching between
  window and full screen included. Discord, OBS and MSI Afterburner/RTSS have not been tested;
  the DLL hooks nothing in DirectX, only the engine's own code, so no conflict is expected.
- Multiplayer: the DLL changes no rules or calculations, but multiplayer has not been tested.
- Steam Deck and Linux (Proton): Proton uses its own version.dll unless told otherwise; set the
  launch option WINEDLLOVERRIDES="version=n,b" %command% (not tested).

Uninstall
---------
Delete version.dll (or, with the ASI download, plugins\shogun2_engine.asi), shogun2_engine.ini,
shogun2_engine.log and, if present, shogun2_frames.csv from the game folder.

Known limits
------------
- Tested on two PCs (above), both with the Steam version and NVIDIA graphics; AMD and Intel
  graphics have not been tested.
- Some antivirus programs may distrust an unsigned DLL that changes a game's code in memory.
  The SHA-256 of the DLL, the same file in both downloads (version.dll and shogun2_engine.asi):
  12b3b76114fdb4268673accf4b45fc0066467de072a10ef56957ffe43d8d9e16
- Windows 11 Smart App Control has not been tested. When it is on, Windows may refuse to load an
  unsigned DLL; the game then does not start at all, as the engine loads version.dll together
  with itself. Delete version.dll, and the game starts as before.

Reporting a problem
-------------------
Send shogun2_engine.log (and the crash dump, if there is one), the game's graphics settings
(DirectX 9 or 11, resolution, vsync), the CPU, the graphics card and the Windows version.
The log lists file paths, which can show your Windows user name (if the game is in your user
folder): look it through before you post it in public. A crash dump holds a copy of the game's
memory: send it privately rather than in a public post.

Version 0.5.2 (test build), 2026-10-25.
