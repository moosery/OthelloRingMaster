# Changelog

All notable changes to OthelloRingMaster are documented here.

---

## [0.4.1] - 2026-07-07

### Fix two path bugs from v0.4.0's build wiring (caught by first build attempt)

- `RingConversion.h` included `OthelloBasicsForCUDA.h` with angle brackets;
  angle-bracket includes don't check the including file's own directory
  (only explicit `-I` paths), and that directory wasn't separately listed
  -- changed to a quoted include, matching the rest of the codebase's
  same-directory-sibling convention.
- `OthelloRingMaster.vcxproj`'s `OthelloBasicsForCUDA` `ProjectReference`
  wrongly had a `..\` prefix copied from a sub-folder project's reference
  pattern. `OthelloRingMaster.vcxproj` lives at the solution root already,
  so the correct relative path has no `..\`.

## [0.4.0] - 2026-07-07

### Add GPU ring<->row-major boundary conversion (RingConversion)

- Added `OthelloBasicsForCUDA/RingConversion.h`/`.cu`: `dev_RowMajorToRing`/
  `dev_RingToRowMajor`, the only place in the whole solution allowed to know
  both bit orderings exist. Table-based (`dev_GatherByRingPermutation`, a
  single primitive reused for both directions via constant-memory forward/
  inverse tables), not the analyzer's original 64-iteration-per-call
  approach -- appropriate now since this is real, permanent per-board GPU
  code, not a one-shot offline tool.
- `OBCuda_InitRingPermutationTables` builds the tables on the CPU (via
  `OthelloBasics/RingPermutation.h`, which never touches an actual board's
  bits) and uploads them to GPU constant memory once at startup.
- Added `OBCuda_TestRingRoundTrip`, a validation kernel checking (a) the
  forward/inverse tables are true inverses of each other across several
  edge-case bit patterns, and (b) the known Othello starting position
  forward-converts to the exact ring-ordered constant already hand-verified
  on the CPU side (`BoardKeyAllocate.cpp`) -- catches a table that's
  internally consistent but simply wrong, which a round-trip check alone
  can't.
- Wired `OthelloRingMaster` up to actually call into the rest of the
  solution for the first time: added the missing `OthelloBasicsForCUDA`
  project reference (transitively pulling in `OthelloBasics`/`Utility`),
  the CUDA build customization import (needed for the final EXE to link
  against the CUDA runtime pulled in by `OthelloBasicsForCUDA.lib`), and
  the necessary include directories. `main()` now runs the round-trip test
  and reports PASS/FAIL instead of just printing a greeting.

## [0.3.0] - 2026-07-07

### Re-scope OthelloBasics / OthelloBasicsForCUDA to the CPU-organizes/GPU-solves boundary

- **BOARD_KEY consolidated**: dropped its next-player bit and padding --
  it's now just the two bitboard fields (16 bytes). Next-player is tracked
  externally (which file/batch a key belongs to) rather than per-record,
  matching the convention `BlasterFile.h`'s `BOARD_KEY_DISK` already used.
- **OthelloBasics gutted to its CPU-safe surface**: `BoardKeyCompare` (now a
  single unified two-term comparator -- the old `BoardCompare`/
  `BoardKeyCompare` split collapsed into one now that both structs compare
  the same two fields), `BoardKeyAllocate`/`BoardKeyAllocateClone`, and a
  reworked `BoardKeyPrint`. Deleted outright: `BoardCreateUniqueBoard`,
  `BoardFlip`, `BoardMirrorVerticalAxis`, `BoardMoveCalculator`,
  `BoardRotate90DegreesRight`, `MovePlayAndSetResultBoard` (all already
  fully mirrored by existing `dev_` GPU functions), plus `MoveAllocate`/
  `MoveSet`/`MovePrint`/the `MOVE` struct and `Rotation.h` (unused once
  their only caller was gone).
- **Starting position hardcoded**: `BoardKeyAllocateFirstBoard` now sets a
  precomputed ring-ordered constant instead of calling row-major
  `SETOCCUPIED`/`SETWHITE`/`SETBLACK` macros and running move-gen at
  startup. The constant is identical across every board size, since the
  center 2x2 starting cells never move regardless of board size.
- **BoardKeyPrint reworked**: prints directly from ring-ordered bits via the
  inverse ring-permutation table as a lookup (`RingPermutation.h`/`.cpp`,
  promoted out of `OthelloRingSplitAnalyzer`) -- never materializes a
  row-major value on the CPU.
- **OthelloBasicsForCUDA absorbed everything row-major**: the full `BOARD`
  struct and every row-major bit macro moved here from `OthelloBasics.h`
  (CPU no longer has access to them at all, not just "doesn't currently use
  them"). Dropped the `_key`-suffixed device function family
  (`dev_applyMove_key`, `dev_canonicalize_key`, etc.) -- it read a
  next-player bit off `BOARD_KEY` that no longer exists, and was for a
  different external consumer (OLE) not part of this solution anyway.
  `BOARD`-based device functions (move-gen, flip, rotate/mirror,
  canonicalize) are unchanged.
- Fixed stale `OthelloBasics.vcxproj` include paths (`../FastInsert`,
  `../BPlusTree` -- leftover from the original Blaster project, don't exist
  here) and added a missing `Utility` project reference.

## [0.2.2] - 2026-07-07

### Move BlasterFile.h/.cpp under OthelloRingSplitAnalyzer

- `BlasterFile.h`/`.cpp` were only ever here so the analyzer could read
  Blaster's real on-disk store format to validate the ring-split theory
  against production data -- not a solution-wide shared format. Moved them
  from the solution root into `OthelloRingSplitAnalyzer/` to reflect that
  actual scope honestly, instead of a premature "shared utility" location.
  Genericizing this into `Utility` is explicitly deferred, not abandoned --
  revisit if/when the ring-split work graduates past an analysis experiment.
- Updated `OthelloRingSplitAnalyzer.vcxproj`/`.vcxproj.filters` paths
  accordingly, and dropped the now-dead bare `..` entry from
  `AdditionalIncludeDirectories` (it existed only so the quoted
  `#include "BlasterFile.h"` could reach the solution root; no longer
  needed now that the header lives alongside its only includer).
- No behavior change.

## [0.2.1] - 2026-07-07

### Extract lz4 into its own project

- Added `lz4/lz4.vcxproj` (static library) so the vendored lz4 source
  (`lz4`/`lz4hc`/`lz4frame`/`xxhash`) lives in its own project instead of
  being compiled directly into whichever project happened to need it. Matches
  the intent (not the letter) of `OthelloLevelBlaster`'s pattern, which
  compiled lz4 directly per-consumer -- quietly duplicating it across
  projects. Here there's exactly one lz4 project, referenced by whoever needs it.
- `OthelloRingSplitAnalyzer` now references `lz4.vcxproj` via
  `ProjectReference` instead of compiling `lz4/*.c` itself.
- No behavior change -- pure build-structure cleanup ahead of the planned
  BlasterFile genericization (moving the generic compressed-record-file
  logic into `Utility`, which will then also depend on `lz4`).

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
