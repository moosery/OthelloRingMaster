# Changelog

All notable changes to OthelloRingMaster are documented here.

---

## [0.2.0] - 2026-07-07

### Add OthelloBasics, OthelloBasicsForCUDA, OthelloRingSplitAnalyzer; BlasterFile format

- Added `OthelloBasics` (board/move/play structures and manipulation
  functions shared by CPU and GPU code) and `OthelloBasicsForCUDA` (the
  CUDA-exclusive mirror), ported from `OthelloLevelBlaster`. Both are kept
  as-is for now -- no trimming yet, since the board representation itself
  (row-major vs. ring-ordered) is about to change; pruning unused code
  before that lands would be premature.
- Added `OthelloRingSplitAnalyzer`, the offline tool that validated the
  ring-gathered nested-index storage design against real production data
  (see project memory for the numbers). Pure reader of already-finished
  store files; makes no changes to the live solve pipeline.
- Added `BlasterFile.h`/`BlasterFile.cpp` at the solution root -- the shared
  on-disk board-key file format (`BOARD_KEY_DISK` + trailer, in plain
  `.blf`, delta+varint `.blfz`, and delta+varint+LZ4 `.blfzl` forms) that
  `OthelloRingSplitAnalyzer` depends on. This was the one missing piece
  blocking the analyzer from compiling; reformatted to this project's C
  style while porting it over.
- Added the `lz4` third-party source (compiled directly, not linked as a
  prebuilt library), matching `OthelloLevelBlaster`'s pattern.

## [0.1.0] - 2026-07-07

### Initial solution scaffold and Utility project port

- Created the `OthelloRingMaster` Visual Studio solution: a fresh, isolated
  solution (separate from `OthelloLevelBlaster`) for building a ring-ordered,
  GPU-native board+outcome tablebase for Othello (see README for the mission).
- Ported the `Utility` subproject from `OthelloLevelBlaster` -- `ArenaMem`,
  `BinarySearch`/`BinSearchLE`, `ClockTick`, `DriveInfo`, `Error`,
  `FileAndDirUtils`, `Logger`, `Mem`, `RWLock`, `SysMemInfo`, `ThreadPool` --
  and reformatted every file to this project's C style (file/function header
  banners, section markers, aligned declarations and comments).
- Set up version control: this code repo (public, matching
  `OthelloLevelBlaster`'s convention) and a separate private repo backing
  Claude's project memory.
