# Changelog

All notable changes to OthelloRingMaster are documented here.

---

## [0.14.2] - 2026-07-07

### Fix v0.14.1 build: add _CRT_SECURE_NO_WARNINGS to OthelloRingMaster

- `OthelloRingMaster.vcxproj` never got `_CRT_SECURE_NO_WARNINGS` added to
  its x64 preprocessor definitions (unlike `OthelloBasics.vcxproj`/
  `Utility.vcxproj`, which already had it from earlier phases) -- the
  original pre-Phase-4 stub only used `printf`/`RingConversion` calls, so
  the gap never surfaced until this phase's ported Blaster code
  (`strcpy`/`strncpy`/`fopen` throughout `OthelloRingMaster.cpp`,
  `LevelSolverThread.cpp`, `MergeFiles.cpp`) started tripping MSVC's
  secure-CRT deprecation warnings, escalated to errors by `SDLCheck`.
  Added the define to both Debug|x64 and Release|x64, matching the rest of
  the solution's existing convention.

## [0.14.1] - 2026-07-07

### Fix v0.14.0 build: pass /Zc:preprocessor to the CUDA host compiler

- CUDA 13.2's CUB (bundled as `cccl`) hard-requires the conforming MSVC
  preprocessor and fatals otherwise. `GpuKernels.cu` is the only `.cu` file
  in the solution that includes `<cub/cub.cuh>` (`GpuInfo.cu`,
  `OthelloBasicsForCUDA.cu`, `RingConversion.cu` don't use CUB), so it was
  the only one that failed -- caught by the user's first real build
  attempt of the completed Phase 4 pipeline.
- Added `-Xcompiler "/Zc:preprocessor"` to `OthelloRingMaster.vcxproj`'s
  `CudaCompile` `AdditionalOptions` (both Debug|x64 and Release|x64).

## [0.14.0] - 2026-07-07

### Wire main() into the real per-level solve loop -- Phase 4 complete

- Rewrote `OthelloRingMaster.cpp`: CLI arg parsing (`--board-size`,
  `--drives`, `--store-drive`, `--store-dir`, `--cache-dir`, `--port`,
  `--compress`/`--compress-store-only`/`--no-compress`, `--lz4-drives`),
  Ctrl+C graceful-shutdown handling, and the real per-level driver loop
  (GPU solve -> merge-writer drain -> flush -> end-of-level merge -> stats
  snapshot -> `_complete` sentinel write), replacing the round-trip-test-only stub.
- The Phase 2 ring boundary-conversion self-test (`OBCuda_InitRingPermutationTables`
  + `OBCuda_TestRingRoundTrip`) now runs once at startup, before `InitSolver`,
  as a real safety gate -- a silently wrong permutation table would corrupt
  every board from that point on with no other symptom, so `main()` now
  fatals (exit 1) if it fails, instead of that being the program's entire purpose.
- Defaults changed from Blaster's: stats port **17532** (not 17432), cache
  dir `C:\OthelloRingMaster\Cache`, store dir `\OthelloRingMaster\Store`.
  Usage text and sentinel magic (`RSF_SENTINEL_STATS_MAGIC`) updated to
  match the `RSF` naming established in earlier steps.
- **This completes Phase 4** (porting the live-solver pipeline from
  `OthelloLevelBlaster`): `OthelloRingMaster` is now a real GPU-solving,
  ring-ordered-storage Othello level blaster, not just an offline
  board-key/ring-conversion toolkit. See project memory for the full
  8-step history (v0.6.0 through this release).

## [0.13.0] - 2026-07-07

### Port StatsListener; add new OthelloRingMasterStatus client project (Phase 4 Step 7)

- Added `StatsListener.h`/`.cpp`: the status thread -- a TCP server on
  `pConfig->statsPort` that responds to STATUS (full human-readable
  progress report: current level, per-drive breakdown, active merge/flush/
  cascade progress, level history table) and STOP (graceful shutdown).
  Near-verbatim port; only the config/state type names and the version
  banner text changed.
- Added the new **`OthelloRingMasterStatus`** project: a tiny standalone TCP
  client (connect, send STATUS/STOP, print response), promoted from
  `OthelloLevelBlasterStatus`. Default port **17532** (distinct from
  Blaster's 17432, so both solutions' status listeners can run concurrently
  on the same dev machine). Zero dependency on the rest of the solution,
  matching Blaster's own precedent. Follows this solution's house
  `.vcxproj` conventions (Unicode charset, 4 configs) rather than copying
  Blaster's older minimal project format, since this is new-to-RingMaster
  project scaffolding, not ported logic.
- Only Step 8 remains: wiring `OthelloRingMaster.cpp`'s `main()` into the
  real per-level loop (replacing the current round-trip self-test).

## [0.12.0] - 2026-07-07

### Port InitSolver: startup sequence and teardown (Phase 4 Step 6)

- Added `InitSolver.h`/`.cpp`: the one-time startup sequence
  (`computeState` buffer sizing, `ScanForResumeLevel` sentinel-based resume
  scan, `cleanUpDrives`/`createDirectories` ephemeral working-directory
  purge/recreate, drive-ledger seeding, thread-pool creation/start/wait-ready)
  and matching `CleanupSolver` teardown. Mechanical rename
  (`BOARD_KEY_DISK`->`UINT64_PAIR`, `BLF*`->`RSF*`, extensions
  `.blf`/`.blfz`/`.blfzl`->`.rsf`/`.rsfz`/`.rsfzl`).
- **Renamed the single-instance mutex** from `Local\OLB_SingleInstance` to
  `Local\OthelloRingMaster_SingleInstance` -- nothing in this project
  should reference Blaster naming, even in an OS-level name nobody but this
  process ever reads.
- With this step, every piece Step 8 needs to wire into `main()` now
  exists: config/state types, machine/GPU probes, seed file, GPU kernels,
  feeder/merge-writer jobs, merge/cross-drive consolidation, and
  startup/teardown.

## [0.11.0] - 2026-07-07

### Port MergeFiles: k-way merge, cross-drive consolidation, end-of-level merge (Phase 4 Step 5)

- Added `MergeFiles.cpp` (~1600 lines, the largest single file in this
  phase): `FlushMergeWriterBuffer` (in-memory k-way merge of one
  merge-writer thread's accumulated GPU flush segments to an `RSF` file),
  `DoCrossDriveIntermediateMerge` (consolidates NVMe writer files onto a
  medium drive, or total-flushes to the store drive if that drive is full),
  and `DoEndOfLevelMerge` (the end-of-level consolidation of every
  remaining writer/intermediate file into a single sorted, deduped store
  file per player). This completes `OthelloRingMaster`'s solver pipeline
  linkage -- `LevelSolverThread.cpp` (Step 4) now has a real
  `FlushMergeWriterBuffer`/`DoEndOfLevelMerge` to call.
- Mechanical port: every merge comparator here (`MergeHeadGreater`,
  `PoolMergeHeadGreater`) does a plain numeric `(hi, lo)` comparison on
  `UINT64_PAIR` -- it never interprets board bits, so the merge is correct
  regardless of the underlying encoding, as long as every input stream uses
  the same one consistently (already-recorded risk in
  project_gpu_reorder_integration_design memory). Renamed
  `BOARD_KEY_DISK`->`UINT64_PAIR`, `BLF*`/`BlasterFileTrailer`->`RSF*`/
  `RSFTrailer` throughout.
- Dropped `InMemDiskHead`/`InMemDiskHeadGreater` from the original file --
  confirmed dead code there (declared, never referenced anywhere).
- **Caught and fixed a real bug during the house-style pass**: the file
  banner's own explanatory text originally read literal `BLF*/BlasterFileTrailer`
  -- the `*/` substring would have prematurely closed the banner comment.
  Reworded to avoid the literal sequence; this is exactly the class of
  mistake the project's stray-comment verification grep exists to catch,
  and it caught it before commit.

## [0.10.0] - 2026-07-07

### Port LevelSolverThread: GPU feeder + merge-writer jobs (Phase 4 Step 4)

- Added `LevelSolverThread.h`/`.cpp`: the two thread-pool jobs that drive a
  level's solve -- `RunGpuFeederJob` (enumerates a level's store files,
  reads them in black-then-white sub-passes through a 4-slot ping-pong
  buffer, feeds batches to `GpuKernels`, flushes when the accumulator lacks
  room) and `RunMergeWriterJob` (D2H-copies a completed GPU flush into the
  thread's two-stack MW buffer, compresses each player's staging area into
  the shared in-memory pool via the new `RSF` writer, or flushes to disk if
  there's no room). Mechanical port -- this file only ever moves opaque
  `UINT64_PAIR` records between file/GPU/pool buffer, never interpreting
  board bits, so nothing beyond the `BOARD_KEY_DISK`->`UINT64_PAIR` /
  `BLF*`->`RSF*` rename was needed.
- Added `MergeFiles.h` (declarations only -- `FlushMergeWriterBuffer`/
  `DoEndOfLevelMerge`, both called by `LevelSolverThread.cpp`).
  `MergeFiles.cpp`'s actual implementation is Step 5; this step does not
  link on its own yet, same as any mid-phase checkpoint in this port.
- **Filled a real gap in `OthelloBasics.h`**: `GetMaxMovesForBoardSize` was
  identified back in Phase 0 planning as one of the CPU-safe pure-lookup
  functions that should carry over unchanged, but was never actually added
  during the gutting pass. Added now since `LevelSolverThread.cpp` needs it
  directly to size GPU batch/accumulator capacity.

## [0.9.0] - 2026-07-07

### Port GpuKernels: accumulator, ExpandKernel ring boundary, CUB sort/dedup (Phase 4 Step 3)

- Added `GpuKernels.h`/`.cu`: the GPU accumulator (two-stack layout, now over
  `UINT64_PAIR` instead of `BOARD_KEY_DISK`), the CUB `DeviceRadixSort`-based
  `SortAndDedupRegion` dedup pipeline (unchanged logic -- it always treated
  boards as opaque sort keys), and `ExpandKernel`.
- **`ExpandKernel` is the one real technical adaptation in this whole phase**
  (everything else is a mechanical rename): it now converts at its own
  boundary, per the already-recorded ring<->row-major design. Incoming
  `UINT64_PAIR` fields are ring-ordered (the store's on-disk format) and get
  converted to row-major via `dev_RingToRowMajor` before building a working
  `BOARD` and calling the existing untouched `dev_boardMoveCalculator`/
  `dev_playMove`/`dev_canonicalize` (no `_key`-suffixed function family
  needed -- that was already deleted in Phase 0). Each canonicalized child's
  fields convert back to ring order via `dev_RowMajorToRing` immediately
  before the scatter-write into `d_accum`, so everything downstream (the CUB
  sort/dedup/compaction) needs no awareness that ring order is involved at
  all -- a bijective permutation preserves equality.
- **Caught and fixed a real gotcha before it became a silent bug**:
  `RingConversion.h`'s forward/inverse permutation tables are `static`
  `__constant__` memory, so `GpuKernels.cu` (a second `.cu` file including
  that header) gets its own independent, uninitialized copy distinct from
  `RingConversion.cu`'s. Added `GpuKernels_InitRingPermutationTables()`
  (private to `GpuKernels.cu`, called internally by `GpuAccumulatorCreate`)
  so this is handled automatically rather than depending on every future
  caller remembering a second init call.
- Not yet wired into a real feeder loop -- lands in Step 4.

## [0.8.0] - 2026-07-07

### Port CreateSeedFile (Phase 4 Step 2)

- Added `CreateSeedFile.h`/`.cpp`: writes the Othello starting position as
  level 0's single seed record via the new `RSF` writer, idempotent on
  resume, then writes level 0's complete sentinel (level 0 has no
  end-of-level merge).
- The one real change from Blaster's version: uses `OthelloBasics`'s
  `BoardKeyAllocateFirstBoard` (a precomputed ring-ordered constant,
  `BoardKeyAllocate.cpp`) instead of Blaster's row-major
  `BoardAllocateFirstBoard` -- the one necessary CPU-side exception to the
  CPU-organizes/GPU-solves boundary, unchanged from how it already worked
  in Phase 0.
- Not yet wired into `main()` -- lands in Step 8.

## [0.7.0] - 2026-07-07

### Port foundation config/state types and standalone utilities (Phase 4 Step 1)

- Added `OthelloTypes.h` (config/state/stats structs, renamed
  `OthelloLevelBlasterConfig/State` -> `OthelloRingMasterConfig/State`; field
  shapes kept as-is since the multi-drive/multi-writer machinery is real
  functionality this project intends to keep), `DriveLedger.h` (per-drive
  space ledger, logic unchanged), `RSFFileName.h` (file-naming helpers,
  renamed from `BlasterFileName.h`/`BLF*` to `RSFFileName.h`/`RSF*`,
  extensions `.rsf`/`.rsfz`/`.rsfzl`; counts-file naming intentionally not
  ported -- out of scope until the win/tie/loss stats phase), and
  `GetMachineInfo.h`/`.cpp` + `GpuInfo.h`/`.cu` + `InitLogger.h`/`.cpp`
  (machine/GPU capability probes and logger setup, near-verbatim -- none of
  these had any Othello- or Blaster-specific coupling to begin with).
- `OthelloRingMaster.vcxproj` gained a direct `Utility` `ProjectReference`
  (these new files use `Logger`/`Error`/`ThreadPool`/`ClockTick`/
  `FileAndDirUtils`/`DriveInfo`/`SysMemInfo` directly, not just
  transitively) and CUDA build settings for compiling `.cu` files directly
  in this project for the first time (`GpuInfo.cu`).
- `Utility.h`'s umbrella include gained `RingStoreFile.h` (added in Step 0
  but not yet wired into the umbrella header).
- No live-solve logic yet -- this is pure scaffolding for Steps 2-8.

## [0.6.0] - 2026-07-07

### Genericize the record-file format into Utility as RSF; delete OthelloRingSplitAnalyzer

Phase 4 (porting the live-solver pipeline from OthelloLevelBlaster) Step 0.

- Added `Utility/RingStoreFile.h`/`.cpp`: a generic on-disk record-file
  format (`RSFWriter`/`RSFReader`, three tiers: `.rsf` plain, `.rsfz`
  delta+varint compressed, `.rsfzl` delta+varint+LZ4), operating on a new
  generic `UINT64_PAIR { uint64_t hi, lo; }` record type instead of any
  Othello-specific key -- `Utility` stays board-representation-agnostic.
  This is a straight port of `OthelloRingSplitAnalyzer`'s `BlasterFile.h`/
  `.cpp` logic (delta+zigzag+varint encoding, LZ4 framing, trailer-magic
  format detection), renamed away from `Blaster`/`BLF` naming throughout
  (new prefix `RSF`, new magic constants) since nothing in this project
  should reference Blaster going forward.
- `Utility` now depends on `lz4` for the first time (`ProjectReference` +
  include path added) -- this was flagged as a deferred step back when
  `BlasterFile.*` was first ported ("revisit if/when the ring-split work
  graduates past an analysis experiment"); this is that moment, since
  Phase 4's ported solver code needs this same read/write-with-compression
  functionality and there's no reason to keep two copies of it.
- **Deleted `OthelloRingSplitAnalyzer/` entirely** (including the now-
  superseded `BlasterFile.h`/`.cpp`) -- its job, proving the ring-split
  nested-index theory against real production data, is done and already
  reflected in `RingNestedIndex.h`/`.cpp`. Removed from
  `OthelloRingMaster.slnx`.
- No behavior change to anything that already worked; this just relocates
  and renames record-file I/O ahead of the rest of Phase 4 depending on it.

## [0.5.0] - 2026-07-07

### Add RingNestedIndex: reusable CellsInUse -> Ring_1 -> Ring_2 -> Ring_3_4 builder/reader

- Added `OthelloBasics/RingNestedIndex.h`/`.cpp`, promoting
  `OthelloRingSplitAnalyzer.cpp`'s analysis-only `Aggregator` into a
  reusable module: `RingNestedIndexBuilder` consumes a sorted, deduped
  stream of ring-ordered `BOARD_KEY`s (via `Process()`) and writes the four
  nested index files with the same validated cascading close/reopen logic
  the analyzer proved out against real production data (28/20/16-bit
  split, `Ring_3_4` groups always exactly 1 member).
- Added `RingNestedIndexReader`, which didn't exist anywhere before: loads
  the four files back into memory and `ExpandAll()`s them into the
  original sorted stream of `BOARD_KEY`s via sequential offset-following
  (`CellsInUse` span via next-entry lookahead, `Ring_1`/`Ring_2` spans via
  each level's own stored `count`) -- the reverse direction the analyzer
  never needed since it only ever built the index, never read it back.
- Dropped the analyzer's dual raw+compressed `CellsInUse` writing
  (`BLFWriterOpenZ`) -- this module only writes raw records; compression
  is already a solved, separate concern (the MW pool's delta+varint+LZ4
  mechanism).
- This is pure CPU-organizing work (counting/comparison/offset bookkeeping
  over already-ring-ordered numeric keys) -- it never touches row-major
  bit structure, per the CPU-organizes/GPU-solves boundary.
- No behavior change to the existing analyzer or GPU boundary conversion.

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
