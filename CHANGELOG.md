# Changes

## 0.5.1 (2026-10-24)

The first public version. Changes since the 0.5 test build:

- A broken or forged .dds header, say in a mod's files, goes to D3DX as before. Up to 0.5 such a header could make the
  texture fast path write past its buffer or loop forever, and the texture statistics could end the game.
- A game folder whose path has 241 characters or more no longer ends the game at start.
- Two threads calling version.dll's functions for the first time at the same moment no longer see one call fail.
- A crash filter of another program that calls ours no longer makes the two call each other until the stack runs out.
- The pack cache keeps the error code of the engine's CreateFileW calls.
- The DLL no longer turns off the thread notifications that its static C runtime needs.
- The settings file and the readme give the vsync numbers as measured: 1.3-3.4 s per load at 120 Hz; 60 Hz has not
  been measured.
- Tests: their minidumps go to `engine\test\build\dumps` instead of the game's folder; new cases for broken .dds
  headers and for a game folder with a long path (0.5 fails both); the texture test fails when it compared nothing.
