# Changelog

All notable changes to OthelloRingMaster are documented here.

---

## [0.32.2] - 2026-07-23

### Fixed a real data-corruption race between flush and consolidation

- **Found by the validation run itself** (exactly what it was for): a fresh disposable-store
  run showed level 15's black board count short by ~1.77 billion boards versus the
  independently-confirmed real production number, and a second run crashed outright:
  `RSFZReadByte: read past end of LZ4 frame -- 'E:\...\writer_black_0002.rsfzl'
  (743615672/3461934990 records read)` -- a file whose trailer claimed 3.46B records but
  whose actual LZ4 stream ran dry after only 743M (21%).
- **Root cause**: `FlushMergeWriterBuffer`'s own output-file index allocation
  (`mwBlack/WhiteFileCount[ti]`, read before writing, incremented only after close) was
  never protected by any lock -- safe historically, since only that one writer thread ever
  touched its own writer's counter. `DoBackgroundConsolidation` (v0.32.0) reads and writes
  that same counter under `imergeCS`, but that lock alone didn't help since the flush side
  never took it. A flush and a consolidation merge for the same (drive, color) could both
  read the counter before either incremented it, both pick the same output file index, and
  both write to that same path concurrently -- corrupting it.
- **Fix**: new `fileIndexCS[MAX_WRITERS][2]` lock, granular per (writer drive, color) so
  unrelated drives/colors stay fully concurrent. `FlushMergeWriterBuffer` now holds its own
  (drive, color) lock for the entire index-pick-through-increment span;
  `DoBackgroundConsolidation` holds the same lock (nested inside `imergeCS`, always
  acquired in that order to prevent deadlock) for its own outIdx-read-through-increment
  span. The two can no longer land on the same index.
- Still not validated against real data -- the disposable test store that exposed this bug
  is now stale (contains the corrupted file and the wrong level 15 count) and needs a fresh
  run from level 0 to actually confirm the fix before this goes anywhere near the real
  6x6 production store.


## [0.32.1] - 2026-07-23

### Documented --memory-limit's real minimum

- Caught immediately when the user tried a first validation run at `--memory-limit 512MB`:
  `Fatal("Not enough RAM for even one merge-writer buffer (12 GB)")` (`InitSolver.cpp:171`,
  `gpuMinBufSize = GPU VRAM * 80%`) -- correct, intentional behavior (a merge-writer buffer
  must hold at least one full GPU flush), just previously undocumented. With 2 fast writer
  drives, keeping both active (rather than silently dropping to one) needs the limit to
  clear roughly double that floor. README's `--memory-limit` entry now states the real
  constraint in concrete terms instead of just "still capped by actual free RAM."
- No code changes -- documentation only.


## [0.32.0] - 2026-07-23

### Background small-file consolidation (new feature)

- **The idea**: with the RAM upgrade in place, the user noticed D:/E: writer files stay
  relatively small (~20GB) against the much bigger 55.3GB buffer, and reasoned that
  proactively merging small files together on otherwise-idle CPU cores -- shrinking both
  fan-in and total volume before `DoEndOfLevelMerge` ever sees them -- should meaningfully
  speed up the (currently dominant, 46-105+ hour) end-of-level merge phase, at negligible
  cost given D:/E: are NVMe (3965-5393 MB/s) and RingMaster only actively used 4 of 32
  available CPU threads.
- **New dedicated thread pool** (`pConsolidationPool`, size 2, one per writer drive) --
  deliberately separate from `pMergeWriterPool` so it never competes with active
  flush-writing for the same threads.
- **`DoBackgroundConsolidation`** (`MergeFiles.cpp`): merges runs of consecutive small
  writer files via the existing `KWayMergeFiles`, triggered right after every flush
  completes. Files at or above `CONSOLIDATION_SIZE_CAP_BYTES` (100GB, adjustable) are left
  alone -- not worth the merge cost once a file is already that large; new incoming files
  start their own fresh consolidation lineage rather than trying to grow an
  already-graduated one further.
- **Coexists with `DoCrossDriveIntermediateMerge` without either needing to know about the
  other**: a new, independent prefix boundary (`mwBlack/WhiteConsolidatedUpTo`) tracks what
  the consolidator has examined, separate from the existing `mwBlack/WhiteFilesConsumed`
  boundary `DoCrossDriveIntermediateMerge` owns -- lets a graduated (already-large) file be
  skipped without violating that other boundary's strict-prefix invariant. Both share the
  existing `imergeCS` lock.
- **Termination safety**: a new `terminateConsolidation` flag, distinct from the global
  `terminateThreads` (Ctrl+C/shutdown) -- set alone at each level's normal solve->merge
  transition so an in-flight consolidation wraps up promptly (deleting only its own
  partial output file; original inputs are untouched either way) rather than running
  arbitrarily long into the transition. Set alongside `terminateThreads` on a real
  shutdown.
- **New `--memory-limit SIZE` CLI flag** (e.g. `--memory-limit 512MB`) -- forces the memory
  budget instead of using real free RAM, for validating this against small levels without
  needing to wait for real multi-trillion-board volume. Wires up `MemoryMode::MM_SPECIFIED`
  (`Utility/SysMemInfo.h`), which already existed but was never exposed on the command line.
- **New observability**, since none of this is worth anything if it can't be watched
  working: per-level `consolidationFilesCreated`/`consolidationFilesRemoved` counters; a
  real-time physical file count (`mwBlack/WhitePhysicalFileCount`, incremented/decremented
  on every create/delete) since the existing file-count fields never decrement and can't
  answer "how many files really exist right now" once consolidation starts creating gaps
  in the index range; and a live `Consol D:/E:` progress line (GB done/total, %, MB/s,
  boards/s, ETA) matching the existing `Flush`/`Merge` line format.
- **Fixed a real diagnostic gap found along the way**: `RSFZReadByte`'s six `Fatal()` calls
  (`Utility/RingStoreFile.cpp`) reported only a bare message with no file path -- caught
  live when a real `Fatal(FATAL_FILE_OPEN, "RSFZReadByte: LZ4 read failed")` fired with no
  way to tell which file failed (root-caused separately to a USB power-management setting
  bouncing the NAS connection, not real corruption). `RSFReader` now carries its own path
  (real path for file-backed readers, a `<in-memory pool segment>` placeholder for
  memory-backed ones), and all six Fatals now include it plus bytes-consumed/total,
  records-read/total, and `ferror`/`feof` where relevant.
- Not yet run against real production data -- validated via a separate, disposable store
  (the live Y: store renamed aside first) before this touches the actual multi-week 6x6 run.


## [0.31.0] - 2026-07-21

### Fixed the per-level "uncompressed" stat to reflect real ring-format size

- **The problem**: `DoEndOfLevelMerge`'s finalize-stats block computed the level's "uncompressed equivalent" as `uniqueBoards * sizeof(UINT64_PAIR)` (16 bytes/board) -- a leftover from the pre-v0.27.0 flat row-major format. This badly overstates true size in the current ring-nested format, since `CellsInUse`/`Ring_2` fold many boards into far fewer group records; the flat-format guess was off by roughly 18x on real level-19 data (18.4TB guessed vs. the real ~5.6-5.8 bits/board `OthelloRingMasterStoreStats` already reports correctly). Found while reconciling a live console reading against `OthelloRingMasterStoreStats`'s CSV output.
- **The fix**: after each color's ring files are written and closed, their trailers are read back and `recordCount * width` summed across all four (`CellsInUse`, `Ring_1`-if-applicable, `Ring_2`-if-applicable, `Ring_3_4`) -- the same computation `OthelloRingMasterStoreStats` already does correctly. Fatals if a just-written file can't be reopened (real corruption, not legitimate absence).
- Display-only fix -- no storage format, resume logic, or correctness path touched. Takes effect for the next level this build completes; historical sentinel-stored levels keep their originally-recorded (flat-formula) figures, since the fix isn't retroactive.

### Doubled MAX_MW_SEGS (512 -> 1024) alongside a RAM upgrade

- The user upgraded the solver machine from 64GB to 128GB RAM (2x32GB -> 4x32GB DDR5), roughly doubling the per-thread merge-writer buffer (26.9GB -> 55.4GB) so more GPU-flush segments accumulate -- and get deduped -- in memory before a flush to NVMe, reducing the volume that reaches the (much more expensive) end-of-level merge.
- `MAX_MW_SEGS` is a fixed bookkeeping-array bound, independent of the RAM-driven byte-capacity check that actually decides when a buffer is "full". Live level-22 data showed segment count already at ~244/512 after only 6 minutes with zero flushes yet -- on track to hit the fixed segment cap well before the doubled byte capacity ever filled, which would have forced flushes at roughly the old cadence and capped the RAM upgrade's real benefit. Caught and fixed before level 22 had made meaningful progress (6 minutes, 0.42% of the level), so nothing was lost by stopping to apply it.


## [0.30.0] - 2026-07-11

### Eliminated a full, wasted level-read on every level transition

- **The problem**: `RunGpuFeederJob`'s pre-scan (for `StatsListener`'s solve-phase % progress) streamed and decompressed an entire incoming level via `RingNestedIndexStreamAll`, just to count records -- on top of `FeedNestedIndexLevel` streaming the exact same data a second time immediately after, for the real work. A deliberate, documented tradeoff when originally written ("an acceptable trade of sequential I/O time for never needing the whole level in memory"), but invisible at small scale and increasingly costly as levels grew -- at level 19->20 (526B+ boards, 355.6GB compressed), this doubled a real, multi-tens-of-minutes read into a silent, easy-to-mistake-for-a-hang wait before any status field updated. Found live, mid-run, while diagnosing exactly that apparent stall.
- **The fix**: `Ring_3_4`'s own trailer `recordCount` already *is* the board count for a given level/player (every `Ring_3_4` group has exactly one member, by construction -- the same invariant `OthelloRingMasterStoreStats` already relies on). The pre-scan now opens each color's `Ring_3_4` file, reads its 64-byte trailer, sums `recordCount`, and closes -- no decompression, no full pass, a couple of near-instant reads instead of reading the entire level. Existence/corruption handling is unchanged: a color with no applicable files still skips cleanly (not an error); a color whose `Ring_3_4` file exists but yields no valid trailer still Fatals with full diagnostic detail.
- Source-only change -- requires a rebuild and restart of the solver to take effect; does not touch stored level data in any way.


## [0.29.3] - 2026-07-11

### Fixed a build break in StoreStatsRing34BitStats.cpp

- Missing `#include "FileAndDirUtils.h"` (`MAX_FULL_PATH_NAME` undeclared) -- caught by the user's own build immediately after v0.29.2.


## [0.29.2] - 2026-07-11

### --ring34-bitstats now prints progress every 5%

- A cheap trailer-only pre-pass sizes the run's total record target (both colors, respecting `--limit`) before the real decompressing read starts, so a large or unbounded (`--limit 0`) run prints a progress line to stderr every time another 5% is crossed instead of sitting silent.


## [0.29.1] - 2026-07-11

### Fixed --ring34-bitstats reading an in-progress level

- `--ring34-bitstats --level N` had no check that level N is actually finished -- pointing it at the level the live solver is currently writing would open a `Ring_3_4` file with no valid trailer yet and misreport it as corrupt (`Fatal(FATAL_FILE_OPEN, ...)`), when it's really just not done. New `StoreStatsLevelIsComplete` (checks the same `_complete` sentinel `StoreStatsFindDeepestCompleteLevel`'s walk already relies on) gives a clear, non-fatal message instead. Caught by the user asking "is this safe to run while it's crunching" before actually running it against the live 6x6 store.


## [0.29.0] - 2026-07-11

### OthelloRingMasterStoreStats: new Ring_3_4 bit-occupancy diagnostic mode

- **New mode**: `--ring34-bitstats --level N [--limit N]` -- unlike the normal per-level CSV table (trailer-only, never decompresses), this mode does real delta+varint+LZ4 decompression of one level's `Ring_3_4` records (both colors) and reports per-bit-position occupancy (how often each of the 16 bits is set), a popcount histogram, and the average popcount -- i.e. how many of the 16 board-cell positions that field covers are actually occupied at a given depth, versus still contributing a fixed-zero (unoccupied) bit.
- **Why**: answers a genuinely different question than compression ratio -- whether `Ring_3_4` still has real *uncompressed* structural headroom at a given level (fewer than 16 bits of true information content, from unoccupied cells), which matters for the retrograde calculator's actual working-set size (it fully decompresses into fixed-width scratch stores regardless of the on-disk compression tier, so only uncompressed-record-width reductions shrink that footprint -- compression-ratio improvements don't).
- New `StoreStatsRing34BitStats.h`/`.cpp`, streaming via the same O(1)-memory `RSFOpenShaped`/`RSFReadShaped` pattern used elsewhere in this project -- never loads a level wholesale.
- `--limit` (default 50,000,000; `0` = unlimited) bounds how many records get read, since the compressed stream can only be decoded sequentially from the start -- a bounded limit reads a *leading* sample (smallest `CellsInUse` patterns first), not a uniformly-random one. For a fully unbiased answer, run unlimited on a level cheap enough to afford a full decode.


## [0.28.3] - 2026-07-10

### OthelloRingMasterStoreStats: added DupsRemovedPercent column

- New column: `DupsRemovedPercent` = `DupsRemoved / BoardsGenerated * 100`, blank under the same conditions as `BoardsGenerated`/`DupsRemoved` (no generation stats, or `BoardsGenerated` is 0).


## [0.28.2] - 2026-07-10

### Fixed DupsRemoved undercounting a real, previously untracked dedup stage

- **Found via investigation prompted by the user noticing the numbers didn't add up**: `BoardsGenerated - DupsRemoved` should exactly equal `TotalBoards` for any level with generation stats, and did for levels 1-15 -- but diverged starting at level 16, growing from ~3.17B boards off to ~109B by level 19.
- **Root cause**: there are three dedup stages in the solve pipeline, and `LevelStats` only has dedicated counters for two of them. `MergePoolToWriter` (`MergeFiles.cpp`) -- which k-way-merges a merge-writer thread's accumulated GPU-flush segments before spilling to an NVMe file -- silently drops cross-segment duplicates (`bool dup = ...; if (!dup) { RSFWriterRecord(...) }`, no counter incremented on the dup branch). This only fires once a thread's pool holds more than one GPU flush's worth of data before it fills up enough to spill -- never happened for levels 0-14 in the real 6x6 run (pool never filled, "0 files + N pool readers" in the log the whole time), first happened at level 15 (log shows real writer files for the first time), and grows with level size since deeper levels accumulate more segments per flush.
- **Fix, retroactive**: `LevelStats` already tracks the two fields that bracket this stage (`boardsReceivedFromGpu`, tallied before it runs; `boardsWrittenToDisk`, reflecting its result) even though neither run of the solver ever explicitly counted the stage itself. `OthelloRingMasterStoreStats`'s `readLevelGenerationStats` now derives it as `boardsReceivedFromGpu - boardsWrittenToDisk` and includes it in `DupsRemoved`, so the fix applies to every level's sentinel already on disk -- no solver-side code change or re-run needed, no new field, purely a tool-side formula fix.
- `BoardsGenerated - DupsRemoved == TotalBoards` now holds exactly for every level with generation stats, confirmed against the real live 6x6 run's own numbers.


## [0.28.1] - 2026-07-10

### OthelloRingMasterStoreStats: added GPU generation/dedup columns from sentinel stats

- **New columns**: `BoardsGenerated`, `DupsRemoved`, `CumulativeBoardsGenerated`. The first two come from the level's own `_complete` sentinel, which embeds the solver's full `LevelStats` for the step that produced it (`WriteSentinelStats` in `OthelloRingMaster.cpp`) -- `BoardsGenerated` is the raw GPU-generated board count before dedup, `DupsRemoved` is `gpuDupsRemoved + mrgDupsRemoved`. `CumulativeBoardsGenerated` is a running total across levels 0..N.
- Reuses `OthelloTypes.h`'s `LevelStats` struct directly (header-only, no new link dependency) for binary-compatible reads, mirroring the read pattern `InitSolver.cpp`'s own `ReadSentinelStats` already uses (magic check via `RSF_SENTINEL_STATS_MAGIC`, then a raw struct read) -- reimplemented locally since that function is file-scope static, not exported.
- Level 0's sentinel is zero-byte (no stats payload, predates any solve step) -- `BoardsGenerated`/`DupsRemoved` are left blank for it rather than reported as zero, distinguishing "not tracked" from "genuinely zero."


## [0.28.0] - 2026-07-10

### New tool: OthelloRingMasterStoreStats -- per-level CSV store statistics

- **New standalone project `OthelloRingMasterStoreStats`**: scans a store directory's ring-store file trailers (no decompression -- every figure comes from each file's 64-byte `RSFTrailer`, i.e. `recordCount`, plus its real on-disk size) and prints one CSV row per completed level: `Level,TotalBoards,WhiteBoards,BlackBoards,CompressedBytes,UncompressedBytes,Ratio,ReductionPercent,BitsPerBoard`. Both colors' CellsInUse/Ring_1(if applicable)/Ring_2(if applicable)/Ring_3_4 files are folded into one set of per-level totals, not reported per-file.
- CLI mirrors `OthelloRingMaster.exe`'s own `--store-drive`/`--store-dir`/`--board-size` flags and defaults (`Y` / `\OthelloRingMaster\Store` / `6`), so running it with no arguments points at the same store a default-configured solver run writes to; `--output PATH` writes the CSV to a file instead of stdout.
- Depends only on `Utility` (for the `RSFOpen`/`RSFOpenShaped`/`RSFReaderTrailer` trailer-only read API) and `OthelloBasics` (for `RingNestedIndexHasRing1/HasRing2`/`RingNestedIndexFileCount`) -- no CUDA dependency at all, unlike `OthelloRingMasterCalculator`.
- Never Fatals on a legitimately-absent early-level single-color file (e.g. level 0 has no white files in real 6x6 data); only Fatals if a file a sentinel already confirms exists fails to yield a valid trailer (genuine corruption).


## [0.27.0] - 2026-07-09

### Eliminated the ring-store double-write; Calculator now searches ring-shaped scratch instead of rehydrated board keys; cascade's own intermediate files become ring-format too

- **The core problem, found by the user reviewing the merge code**: every level was written TWICE. `DoEndOfLevelMerge` k-way-merged into a flat, sorted+deduped `.rsf`/`.rsfz`/`.rsfzl` file, then `ConvertLevelOutputToNestedIndex` immediately reread that whole file and rewrote it as the ring nested-index format (`CellsInUse`/`Ring_1`/`Ring_2`/`Ring_3_4`), deleting the flat intermediate afterward. Real, measured doubled I/O against the actual per-level store data, not just a theoretical inefficiency.
- **Fix -- merge writes ring format directly, once**: new `KWayMergeFilesToRingIndex` and a `pRingBuilder` parameter on `CascadingMerge` feed the merge heap's deduped, sorted output straight into a `RingNestedIndexBuilder` instead of a flat `RSFWriter`. `DoEndOfLevelMerge`'s `mergePlayer` now opens the level's 4 ring files up front and merges directly into them -- no flat intermediate file, ever, for a level's store output. `ConvertLevelOutputToNestedIndex` and the old single-file "just move it" fast path (no longer a valid shortcut once output is structurally different from input) are deleted.
- **All four ring files now share one compression tier**: `Utility/RingStoreFile.h`/`.cpp` gained a generalized shaped API (`RSFRecordShape`: `RSF_SHAPE_PAIR64`/`RSF_SHAPE_RING_LEVEL`/`RSF_SHAPE_LEAF16`, plus `RSFWriterOpenZLShaped`/`RSFWriterRecordShaped`/`RSFOpenShaped`/`RSFReadShaped`) added alongside the existing `UINT64_PAIR`-based API, which is completely unchanged in signature and behavior. `Ring_1`/`Ring_2`/`Ring_3_4` now go through the same delta+varint+LZ4 machinery `CellsInUse` (and the flat store format) already used, instead of the separate, simpler `Lz4Stream` framing. `RingLevelRec` (`Ring_1`/`Ring_2`'s shared shape) shrinks from 20 to 12 bytes -- dropped the `count` field, derived instead via one-record lookahead exactly like `CellsInUse` already worked (offsets are monotonic across each level's whole flat stream, not just within one parent group, so the same lookahead trick generalizes cleanly). Ring filenames now carry the tier explicitly (`.ring1.rsfzl`, `.ring2.rsfzl`, `.ring34.rsfzl`, `.cellsinuse.rsfzl`) so it's visible alongside the role.
- **Calculator's lookup source no longer rehydrates to flat board keys**: previously staged level+1's data as one drive-segmented store of flat, expanded 16-byte `BOARD_KEY` records (one per board) -- real space cost, since the ring format's hierarchical grouping is inherently smaller even fully decompressed. Now stages the DECOMPRESSED but still ring-SHAPED records across 4 segmented stores (`CellsInUse`/`Ring_1`/`Ring_2`/`Ring_3_4`, skipping Ring_1/Ring_2 per board size as always), each built via one straight decompress-and-rewrite streaming pass -- no rehydration at all. `LookupChildTriple` now walks the hierarchy (`CellsInUse` -> `Ring_1` -> `Ring_2` -> `Ring_3_4`) the same way `RingNestedIndexReader::FindBoardPosition` already does in memory, just against drive-segmented stores. Enabled by two new pieces: `Utility/BinarySearch.h`'s `BinarySearchFile` gained an optional `startIdx` (fully backward-compatible, defaults to 0) to restrict a search to a sub-range of a file's record array; `SegmentedStoreReader::FindPatternInRange` uses it for a fast single-segment search when a group's range falls inside one physical segment (the common case), falling back to a manual `ReadAt`-driven search when a range straddles a segment boundary (rare, still correct). `SegmentedStoreReader::FindByKey` and the `isKeySorted`/min-max-key tracking it needed are deleted -- dead now that nothing writes a flat key-sorted store any more.
- **Cascade's own intermediate/grouped merge files become ring-format too** (only relevant when a level's merge-input file count exceeds `MAX_MERGE_FANIN`): new `RingNestedIndexPullReader` (`Peek()`/`Advance()`) is a pull-style counterpart to the existing push-style `RingNestedIndexStreamAll`, needed so a k-way merge heap can interleave several ring-format inputs at once. It mirrors the builder's own `CloseRing1Group`/`CloseRing2Group` cascade in reverse -- running consumed-record counts per level, cascading a read cursor upward exactly when the count reaches the next record's stored offset -- with corruption detection consolidated to the one place it can actually happen (`Ring_3_4`, the leaf, hitting EOF while a higher level still promises more). New `CascadeGroupsToRingIndex` groups cascade inputs exactly like the existing flat path (the drive-selection logic itself is factored into a shared `ChooseNextCascadeGroup`, used by both), writes each group as 4 ring files via `KWayMergeFilesToRingIndex`, then merges those groups directly into the final builder via `MergeRingGroupsIntoBuilder`. Deliberately single-round: if a level's groups themselves ever exceed `MAX_MERGE_FANIN` (would need well over 12 million real input files for one level/color -- the largest real run on record needed ~42,000), this Fatals with a clear diagnostic rather than running an untested recursive-cascade path that's never been exercised at this project's scale.
- **Standing constraint respected**: this is a breaking on-disk format change, developed and intended for a fresh run -- not applied to the currently-live 6x6 solve.


## [0.26.3] - 2026-07-09

### Fixed live status query's mislabeled drive columns (merge dir / store drive rows)

- **The bug**: `StatsListener.cpp`'s live `STATUS` response (`OthelloRingMasterStatus.exe`) mislabeled the merge-dir (F:) and store-drive (Y:) rows in its per-level drive breakdown table -- a stray hardcoded `1` printed where the real file count should go, and the actual board/file count (`blk + wht`) printed in the "Disk GB" column position with no `GB` suffix and no byte conversion (visible in a real run as `F   1   0` instead of a sensible file count and byte size). In the non-zero-disk branches, the REAL byte total ended up shifted into the "Uncomp GB" column instead of "Disk GB". The writer-drive rows (D:/E:) were never affected -- only F:/Y:.
- **Confirmed NOT a data-loss or solve-correctness bug**: cross-checked the real files on disk against both the (correctly-coded, separate) log file's own per-level drive summary and the live query -- the actual stored data and the log file's own byte counts were accurate throughout; only this one live-query table's column layout was wrong.
- Fixed by removing the stray hardcoded `1` argument and using `blk + wht` for the Files column (matching the writer-drive rows' existing, correct shape) in all six affected `snprintf` call sites (three format variants x two sections: merge dirs, store drive).


## [0.26.2] - 2026-07-09

### Removed the remaining "no X-to-move boards" log line

- Removed `"level %d has no %s-to-move boards, skipping"` from `TerminalLevelBootstrap.cpp`/`NonTerminalLevelStep.cpp` -- the last leftover from the pre-table log style (v0.26.0 already removed the per-color/per-level summary sentences). A color genuinely having zero boards at a level is expected, not exceptional: verified against `GpuKernels.cu`'s pass-handling (`ExpandKernel`) that a 100%/0% color split at a level is a structurally normal outcome of the game tree, not data loss -- level 1 is 100% white-to-move (black always has a legal opening move), and level 2 came out 100% black-to-move in a real 4x4 run because none of level 1's boards triggered a pass either.


## [0.26.1] - 2026-07-09

### Phase 6 validated (4x4 matches known B=24632/W=30116/T=5312), cleaner log output, and a duration row

- **Phase 6 validation passed**: a `--force` 4x4 run's `FINAL RESULT` box exactly matched the known-correct totals for the first time. The retrograde calculator's core algorithm is now confirmed correct end to end.
- Removed the per-color and per-level `ProcessTerminalLevel`/`ProcessNonTerminalLevel` `LoggerLog` sentences -- redundant now that the scrolling table (`v0.26.0`) shows the same boards/wins/ties/duration data in a cleaner form.
- Fixed the `FINAL RESULT` box's border alignment: the title's top border was 2 characters narrower than the body's middle borders (a miscounted width that omitted the middle divider), making the box look visibly crooked. All borders now compute from the same label/value column widths.
- Added a `Duration` row to the `FINAL RESULT` box -- the whole backward walk's wall-clock time (deepest level down to 0), formatted `H:MM:SS` via new shared `CalcFormatDurationHMS` (`CalculatorLevelTable.h`/`.cpp`).


## [0.26.0] - 2026-07-09

### Table-ized per-level logging, matching RingMaster/Blaster's scrolling table

- New `OthelloRingMasterCalculator/CalculatorLevelTable.h`/`.cpp`: a shared column-spec table (`Lv`, `Boards`, `BlkWins`, `WhtWins`, `Ties`, `Terminal`, `Width`, `Dur(s)`, `Brd/s`, `ns/brd`, `CompletedAt`) driving both the header/separator builder and the per-row formatter, so header and data can never drift out of alignment with each other. Used by both `BackwardWalkDriver.cpp` (logged to screen and log file as each level completes, plus a full reprint under `--- Completed level history ---` at the end of the walk -- mirroring `OthelloRingMaster.cpp`'s own scrolling table exactly) and `CalculatorStatsListener.cpp`'s on-demand STATUS history table, so the log and the live query always agree on what the columns mean.
- Two new `CalculatorLevelStats` fields feed the new columns: `terminalBoards` (boards at this level with zero legal moves, classified directly -- always equals total boards for a terminal level, however many happened to have no moves for a non-terminal one) and `counterByteWidth` (this level's confirmed tier width). Both are part of the sentinel's persisted stats payload, so they survive a resumed run like everything else added in v0.25.5.
- `BackwardWalkDriver.cpp`'s end-of-run `FINAL RESULT` line is now a small ASCII box (black wins / white wins / ties / total games), replacing the plain log sentence.


## [0.25.6] - 2026-07-09

### New --force option to force a clean re-run

- `OthelloRingMasterCalculator` gains `--force`: deletes this board size's own `_calc_complete` sentinels and `.counts` files (a single wildcard sweep on the `Level_NNNN_WxH_` filename prefix shared by both) before starting, so every level is genuinely reprocessed instead of skipped as already-complete. Only matches the exact board size being run -- `countsDir` can legitimately hold more than one board size's results at once, since board size is embedded in every filename rather than the directory. RingMaster's own store (read-only input under `--store-dir`) is never touched. The width-config cache (`counterwidthconfig_WxH.json`) is deliberately left alone -- its learned minimum-safe-width knowledge is still valid even when recomputing counts, and clearing it would just cause wasted overflow-retry churn re-learning the same widths.


## [0.25.5] - 2026-07-09

### Shared drive-benchmark cache with RingMaster, and sentinel-persisted level stats (resume no longer loses the FINAL RESULT)

- **Shared drive benchmark cache**: caught by the user -- "the benchmark from the RingMaster should be equivalent to the Calculator, right? So we really don't need a separate file for the benchmark results." New `OthelloRingMasterCalculatorConfig::driveCacheDirName` (`--drive-cache-dir`, defaults to `C:\OthelloRingMaster\Cache`, RingMaster's own default cache dir) is now what `GetDriveInformation`'s `pCacheDir` argument uses, instead of the calculator's own `cacheDirName`. Write/read MB/s is a property of the physical drive, not of which program is asking, so the calculator now reuses RingMaster's already-benchmarked `driveinfo.json` instead of re-benchmarking every drive from scratch. Deliberately NOT just pointing the calculator's whole `cacheDirName` at RingMaster's -- both programs' log files use the identical `log_WxH_date.txt` naming pattern, so sharing the full cache directory would risk one silently overwriting the other's log.
- **Sentinel-persisted level stats, matching RingMaster's own resume behavior**: the calculator's `_calc_complete` sentinel was a zero-byte marker, so a resumed/rerun invocation could never report a previously-completed level's real numbers (including the new FINAL RESULT line) -- it could only skip reprocessing it. New `WriteCalcSentinelStats`/`ReadCalcSentinelStats` (`CalculatorFileName.h`, own `CALC_SENTINEL_STATS_MAGIC`) mirror RingMaster's own `WriteSentinelStats`/`ReadSentinelStats` (`OthelloRingMaster.cpp`/`InitSolver.cpp`) exactly: the sentinel now carries the full `CalculatorLevelStats` payload, restored into `pState->levelStats[level]` whenever `BackwardWalkDriver.cpp` skips an already-complete level. A legacy zero-byte sentinel (written before this version) is detected and never silently reported as a real (all-zero) result -- `RunBackwardWalk` distinguishes "stats restored" from "sentinel present but no stats payload" and only prints FINAL RESULT in the former case.


## [0.25.4] - 2026-07-09

### Per-level win/tie/loss totals are now exact (not terminal-only), plus a final-tally line and a drive-safety fix

- **The real gap**: per-level `blackWins`/`whiteWins`/`ties` totals only ever counted boards directly terminal AT that level (a one-hot `if (isTerminal) blackWins++`), silently ignoring the actual recursively-summed value already computed and persisted for every non-terminal board. Since most boards at most levels are non-terminal, this total was near-meaningless everywhere except the single deepest level -- and for level 0 specifically (always exactly one board, always non-terminal, the true starting position) it always showed zero, even though the real, fully-validated answer was sitting right there in that board's own computed triple.
- New `OutcomeTriple.h`/`.cpp`: `WinTieLossTripleAccumulateNibble`/`WinTieLossTripleAccumulateWide` -- adds a board's own final value (terminal or summed) into the level's running `uint64_t` display total, Fatal (never silent truncation) if a single value needs more than 8 bytes or if the running sum would overflow uint64_t -- unreachable at any board size this project runs today, and a deliberate signal to revisit this display mechanism if it ever does fire at real 8x8 scale.
- `TerminalLevelBootstrap.cpp`/`NonTerminalLevelStep.cpp` now accumulate every processed board's own value this way, not just terminal ones. For level 0 this makes the level's `combinedTotals` the exact, fully validated final game-tree result the moment the whole backward walk reaches it.
- `BackwardWalkDriver.cpp`'s `RunBackwardWalk` now logs a `FINAL RESULT` line once the walk reaches level 0 -- resume-safe: only printed when level 0 was actually (re)processed by THIS run, not silently assumed valid when a prior run had already completed it.
- Removed now-stale "best-effort display approximation" wording from `PlayerLevelResult`, `TerminalLevelBootstrap.cpp`, and `CalculatorStatsListener.cpp`'s live status text -- these totals are exact now, not approximated.
- **Drive-safety fix, caught by the user**: `OthelloRingMasterCalculator`'s default `--use-drives` was an empty string, which `GetDriveInformation` interprets as "auto-enumerate every fixed local drive" -- including C:, the boot/system drive, which RingMaster's own default (`DEFY`) never touches. Fixed the Calculator's default to match RingMaster's exactly.


## [0.25.3] - 2026-07-09

### NonTerminalLevelStep's completion log lines were missing win/tie/loss counts

- Caught by the user's first clean 4x4 run: `TerminalLevelBootstrap.cpp` already logged per-color and combined black/white/tie totals on every level's completion (per the already-agreed Phase 5 design -- "per-level completion log line: boards processed, total wins for the level, both per-color and combined, duration"), but `NonTerminalLevelStep.cpp` -- which handles every level except the deepest -- only ever logged board counts, never the win/tie/loss numbers. Fixed both of its `LoggerLog` calls (per-color, and the level-complete summary) to match `TerminalLevelBootstrap.cpp`'s exact format.
- Fixed also this turn (build-fix, not a new feature): `CounterWidthConfig.cpp`'s `newLabel` was accidentally scoped inside the previous turn's overflow-only `if` block, but the shallower-level propagation loop below still needs it -- hoisted back out; the propagation log line's own missing trailing `\n` was fixed at the same time.


## [0.25.2] - 2026-07-09

### First real 4x4 run: fixed a drive-ledger exhaustion crash, a level-scan bug, a misleading log message, and a CLI ergonomics gap

- **Drive-ledger exhaustion crash (the real bug)**: `SegmentedStoreWriter: every available scratch drive is full` -- `ReserveNextScratchDrive` grabs a whole drive's entire remaining ledger per segment, and that reservation previously wasn't given back until the segment file was later deleted via `DeleteSegments` (for the lookup-source's board-key/counts writers, that's not until `ReleaseLookupSource`, at the very end of a level's entire processing). A single level needs up to 6 of these datasets alive at once (4 for the lookup source across both colors, 2 for the level's own output), but with only a few usable scratch drives, the ledger ran dry even though every dataset involved was only a few KB. Fixed in `SegmentedStore.cpp`'s `CloseCurrentSegment`: reclaim a segment's unused reservation the moment it closes (once its real size is known), correcting the writer's own `plan` entry to match so a later `DeleteSegments` reclaims only what's actually still spent.
- **Calculator picked the wrong deepest level**: `StoreLevelScan.cpp`'s `FindDeepestCompleteLevel` trusted the deepest `_complete` sentinel it found, but RingMaster (`OthelloRingMaster.cpp`) writes a level+1 sentinel purely to record "confirmed nothing past here" the moment its own solve loop produces zero boards for a level -- not a claim that level+1 has real data. New `LevelHasBoardData` helper steps back past any such empty level(s) to the deepest one with actual ring-index files.
- **Misleading "overflowed" log message**: `CounterWidthConfigBumpLevel` unconditionally logged "level N overflowed at X, widening to Y" even when called just to propagate an already-confirmed width to shallower levels (no real overflow) -- now only logs/writes when `newByteWidth` is actually wider than what's already set. Also fixed a missing trailing `\n` that ran this message together with the next log line.
- **CLI ergonomics**: `OthelloRingMasterCalculator`'s `--store-dir` now appends `\storeDir` itself, matching `InitSolver.cpp`'s own convention for RingMaster's `--store-dir` -- the same value can now be passed to both executables instead of needing the extra `\storeDir` suffix only on the calculator's command line.
- Removed a debug `printf` the user had temporarily added to `RSFFileName.h`'s `SentinelNameComplete` while diagnosing the level-scan issue.


## [0.25.1] - 2026-07-08

### Buffered scratch writes -- larger sequential fwrites instead of one per record

- `SegmentedStoreWriter::Write` (`OthelloRingMasterCalculator/SegmentedStore.h`/`.cpp`) now accumulates records in an in-memory `writeBuffer` and issues one real `fwrite` per `SEGMENTED_STORE_WRITE_BUFFER_BYTES` (32 MB) chunk instead of one `fwrite` per record -- larger sequential writes measurably lower I/O time on both NVMe and HDD tiers. The buffer is a fixed constant, never scaled to level/dataset size (same category of exception as `GpuKernels.h`'s GPU batch size), so it stays consistent with the standing "never hold a whole level resident" rule -- at most a small handful of `SegmentedStoreWriter`s are ever alive at once (confirmed: one level's own output writer, plus level+1's board-key/counts scratch writers while those are being built, never overlapping with the level's own output writer since `LoadLookupSource` fully finishes before a level's own processing starts), so even several 32 MB buffers at once is a trivial fraction of available RAM. Segment-rollover/budget accounting (`currentBytesUsed`) and min/max-key tracking are unaffected -- both already operated per-record on the caller's data, not on when bytes actually hit disk. `Finish()`/segment-close flush any remaining buffered bytes before closing the file, so no data is ever lost on a segment boundary or at end-of-stream.

## [0.25.0] - 2026-07-08

### Streaming nested-index reads everywhere, and on-demand (no-upfront-count) scratch drive reservation

- **The problem this closes**: caught directly by the user ("Oh my goodness no. No file will ever fit fully in memory (except for very small board levels). You shouldn't be doing that ANYWHERE.") -- v0.24.0's board-key scratch build still went through `RingNestedIndexReader::Load()`/`ExpandAll()`, which buffers every record of a level into `std::vector`s before handing any of it back. That's the same wholesale-load problem the scratch-storage system was built to eliminate, just moved one layer down.
- New `OthelloBasics/RingNestedIndex.h`/`.cpp`: `RingNestedIndexStreamAll` -- walks `CellsInUse`/`Ring_1`/`Ring_2`/`Ring_3_4` in lockstep and calls back once per board, genuinely O(1) memory regardless of level size (at most a 1-record `CellsInUseRec` lookahead plus one record each of whichever ring levels apply -- `RingLevelRec` already carries its own child count, so only the very last `CellsInUse` group needs "consume until EOF" instead of a total count). Replaces `Load()`/`ExpandAll()`/`GetBoardCount()` at every call site that reads a level for processing -- both in the Calculator (`CalculatorLookupSource.cpp`, `TerminalLevelBootstrap.cpp`, `NonTerminalLevelStep.cpp`) and in RingMaster's own production forward solver (`LevelSolverThread.cpp`'s `FeedNestedIndexLevel` and its `RunGpuFeederJob` pre-scan), plus a resume-scan validation pass in `InitSolver.cpp` that was calling `Load()` purely to check the files read cleanly -- found during this same audit, not previously flagged.
- `RingNestedIndexReader::Load()`/`ExpandAll()`/`GetBoardCount()` remain, used only where a level is provably tiny by construction (`CreateSeedFile.cpp`'s level-0 single-board seed).
- **`SegmentedStore.h`/`.cpp` redesigned around on-demand drive reservation**: `PlanScratchDrives` (which needed a dataset's total byte count up front) is gone, replaced by `ReserveNextScratchDrive` -- picks and reserves one drive's entire remaining budget at a time, called internally by `SegmentedStoreWriter` the first time it needs a segment and again whenever the current one fills. `SegmentedStoreWriter::Init` no longer takes a plan or a count at all. This was a self-motivated follow-on to the user's memory-efficiency mandate: requiring a count up front had been forcing awkward two-pass (count-then-write) workarounds at several call sites, which is now unnecessary.
- `CalculatorScratchCounts.h`/`.cpp` (`ScratchCountsWriter::Init`) and every writer call site (`CalculatorLookupSource.cpp`, `TerminalLevelBootstrap.cpp`, `NonTerminalLevelStep.cpp`) updated to the no-count `Init` signature. `CalculatorLookupSource.cpp`'s board-key scratch build collapsed from two streaming passes (count, then write) to one, now that no count is needed up front. `TerminalLevelBootstrap.cpp` keeps its own streaming count-only pre-pass -- the one legitimate remaining exception, needed only for the status listener's "% done" denominator (`pStats->totalBoardsBlack/White`), not for drive planning.

## [0.24.0] - 2026-07-08

### Drive-spanning segmented scratch storage: level+1 lookups and level output no longer need to fit in memory

- **The problem this closes**: `LoadNextLevelLookupData`/`CalculatorCountsFile` (Phases 1-3) loaded an entire level's board-key index and counts array wholesale into `std::vector`s. Fine at 4x4 scale, but genuinely impossible at real 6x6 scale -- caught directly by the user ("you can't read the whole N+1 ring information into memory. That's terabytes of information").
- New `CalcDriveLedger.h`: per-drive scratch-space ledger (reserve/reclaim/available), mirroring `OthelloTypes.h`'s own `DriveLedger.h` exactly, sized with the same ~20 GB safety margin RingMaster uses for solving (not `Utility/DriveInfo.h`'s larger 200 GB margin, which serves a different purpose).
- New `SegmentedStore.h`/`.cpp`: a drive-spanning segmented store -- one logical sorted/ordered dataset split into contiguous pieces (no hashing) across drives, fastest tier first (`DRIVE_CAT_FAST`, then `MEDIUM` only once FAST is full, then `SLOW` only once `MEDIUM` is too, reusing `Utility/DriveInfo.h`'s existing classification). Segments are plain, uncompressed, fixed-size-record files -- unlike every other on-disk format in this solution, trading disk space for direct `fseek`-able random access, since this is fast scratch, not permanent storage. Key-searchable lookups ride `Utility/BinarySearchFile` (already existed, previously unused) for the within-segment search; positional lookups are a direct seek. Reader is thread-safe by construction (each call opens/reads/closes its own file handle, touching only the small read-only in-memory segment index otherwise).
- New `CalculatorScratchCounts.h`/`.cpp`: the output-side wrapper -- `ScratchCountsWriter` writes a level's own result to scratch (promoting nibble-tier data to a lossless 1-byte-per-counter scratch record, since scratch is never compressed); `JoinScratchCountsToFinal` reads every segment back in original order -- "read the first drive, write it out, read the next drive, etc.," no sort needed -- and writes the permanent, compressed `CalculatorCountsFile`/`NibbleCountsFile` to the counts directory on Y:, exactly as before.
- New `CalculatorLookupSource.h`/`.cpp`: the read-side wrapper -- stages level+1's board-key and counts data as segmented scratch instead of loading either wholesale. Disclosed, not-yet-closed limitation: building the board-key scratch still requires one transient pass through `RingNestedIndexReader::Load()`/`ExpandAll()` (itself not streaming) to decompress and reshard the ring files; that peak is freed the moment resharding finishes, but not eliminated. The counts side has no such gap -- the permanent counts file is already read sequentially, one record at a time, straight into scratch.
- `TerminalLevelBootstrap.cpp`/`NonTerminalLevelStep.cpp` reworked to write via `ScratchCountsWriter`/join instead of directly to the permanent counts file, for every level regardless of kind (deliberately, per the user: "a consistent way for all boards").
- **Parallelized per-parent lookups**: previously fully serialized (a single for-loop doing in-memory vector lookups); now each parent's full set of child lookups+color-swap+sum is dispatched as its own thread-pool job (new `pState->pLookupThreadPool`, sized to CPU core count), since each parent's accumulator is naturally thread-local and lookups now involve real disk seeks where serializing would squander the entire point of spreading data across drives.
- `CalculatorTypes.h`: added `useDrives`/`scratchDirNameNoDrive` config (new `--use-drives`/`--scratch-dir` CLI options) and `driveInfo`/`driveLedger`/`pLookupThreadPool` state, initialized in `main()` via `GetDriveInformation` (same probe/benchmark/cache RingMaster itself uses).
- New `FATAL_DRIVE_SPACE`-based Fatal in `PlanScratchDrives` if all available drives combined can't cover a level's scratch requirement -- never a silent shortfall.

## [0.23.0] - 2026-07-08

### Calculator Phase 5: status/monitoring

- New `OthelloRingMasterCalculator/CalculatorStatsListener.h`/`.cpp`:
  a TCP status thread mirroring `StatsListener.cpp`'s query-on-demand
  shape exactly -- same protocol the already-scaffolded (Phase 0)
  `OthelloRingMasterCalculatorStatus` client expects (connect, send
  `STATUS\n` or `STOP\n`, read the response until the connection
  closes). Response shows the current level's live progress (per color
  and combined win/tie/loss totals, % done, ETA) plus a history table of
  completed levels walked in processing order (deepest level first, the
  opposite of `OthelloRingMaster`'s own 0-first table, since that's the
  direction this calculator actually works in). `STOP` sets
  `terminateThreads`; `BackwardWalkDriver.cpp` now checks it between
  levels (never mid-level, matching the project's whole-level-granularity
  resumability model -- there's no finer-grained stopping point to offer).
- `CalculatorTypes.h`: added `totalBoardsBlack`/`totalBoardsWhite` to
  `CalculatorLevelStats` (the live "% done" denominator, set as soon as a
  color's board count is known) and `deepestLevel` to
  `OthelloRingMasterCalculatorState` (the walk's starting point, for
  display). `TerminalLevelBootstrap.cpp`/`NonTerminalLevelStep.cpp`
  updated to populate these fields, and to update `boardsProcessedBlack`/
  `boardsProcessedWhite` incrementally (per board in Phase 2, per GPU
  batch in Phase 3) rather than only once at the end -- the same
  plain-field-read-concurrently pattern `OthelloRingMaster`'s own
  `LevelStats.boardsReadFromStore` already uses, not `std::atomic`.
- `main()` now creates a 1-thread stats pool, submits the listener job,
  runs the backward walk, then stops the pool -- mirroring
  `InitSolver.cpp`'s own thread-pool lifecycle pattern.
- Per-level completion log lines and width-overflow abort/retry logging
  were already in place from Phases 2-4 (`LoggerLog` calls in
  `ProcessTerminalLevel`/`ProcessNonTerminalLevel`/`CounterWidthConfigBumpLevel`) --
  no changes needed there.

## [0.22.0] - 2026-07-08

### Calculator Phase 4: the full backward walk driver

- New `OthelloRingMasterCalculator/BackwardWalkDriver.h`/`.cpp`:
  `RunBackwardWalk` loops every level from the deepest completed level
  down to 0, dispatching the deepest level to Phase 2's terminal
  bootstrap and every level below it to Phase 3's non-terminal step,
  threading one `CounterWidthConfig` through the whole walk so width
  propagation carries across levels correctly.
- Whole-level-granularity resumability: a new per-level sentinel
  (`CalcSentinelNameComplete` in `CalculatorFileName.h`, mirroring
  `RSFFileName.h`'s `SentinelNameComplete` but for this project's own
  counts output) marks a level done only after it fully completes. A
  level with no sentinel is (re)processed from scratch on the next
  run -- deliberately simpler than `InitSolver.cpp`'s own
  `ScanForResumeLevel`, since there's no merging-in-progress state to
  distinguish and no explicit cleanup needed (reprocessing a level just
  naturally overwrites whatever partial output a crash left behind, the
  same way the in-process width-overflow retry already relies on).
- Documented, not actively guarded-against, assumption: this resumability
  assumes RingMaster's forward solve for the store being read is already
  fully complete (matching the existing Phase 7 sequencing note) --
  the deepest completed level must not change between calculator runs
  against the same store.
- `OthelloRingMasterCalculator.cpp`'s `main()` now runs the complete
  backward walk in one invocation instead of stopping after one
  non-terminal level.

## [0.21.1] - 2026-07-08

### Retrograde kernel: positional child color instead of a stored per-child tag

- `RetrogradeKernels.h`/`.cu`: reworked the per-parent child output layout
  from one combined slot range with a stored `d_childPlayer` byte per
  child into a two-stack-per-parent layout -- black children packed from
  the front of the range, white from the back, growing toward each other
  (no atomics needed, since each thread only writes its own range).
  Matches the pattern `GpuKernels.cu`'s own forward-solve accumulator has
  used from the start (its `d_accum` two-stack, split by
  `d_blackWritePos`/`d_whiteWritePos` -- confirmed by inspection that it
  never stored a per-child color tag either, so no equivalent change was
  needed there). Color is now purely positional/derived, matching how
  `BOARD_KEY` itself carries no next-player bit. `RetrogradeGetChildCount`
  is now per-color; the old `RetrogradeGetChildPlayer`/`RetrogradeGetChildren`
  pair is replaced by `RetrogradeGetChild(pCtx, parentIdx, player, childIdx, ...)`.
- Real (if modest) motivation, per direct discussion with the user: this
  isn't about disk space (the eliminated array never touched disk at
  all) or about sharing GPU memory with a concurrently-running
  `OthelloRingMaster` process (the two never run at the same time by
  design) -- it's headroom within the calculator's own process, letting
  its GPU batch size grow for the same memory budget.
- The color-flip tag (`d_childColorFlipped`) is unchanged and still
  per-child -- it answers a different question than which color a child
  is (whether that child's *already-computed, on-disk* triple needs
  black/white un-swapped before summing), with no positional equivalent
  found for it.
- `NonTerminalLevelStep.cpp` updated to consume the new per-color API:
  iterates black then white children per parent instead of one combined
  list with a per-child player check.

## [0.21.0] - 2026-07-08

### Calculator Phase 3: the non-terminal backward step

- New `OthelloBasics/RingNestedIndex.h`/`.cpp`: `RingNestedIndexReader::
  FindBoardPosition` -- finds a board's ordinal position among a level's
  boards (the same index `ExpandAll` would deliver it at) by binary-
  searching down the `CellsInUse -> Ring_1/Ring_2 -> Ring_3_4` hierarchy,
  instead of a full `ExpandAll` walk. This resolves the design's open
  "flat binary search vs. nested-index hierarchy" lookup-mechanism
  question in favor of reusing the same compact, already-resident nested
  representation for lookups too, rather than materializing a separate
  flat array.
- New `OthelloRingMasterCalculator/RetrogradeKernels.h`/`.cu`: the GPU
  side of the backward step. One thread per parent board (mirroring
  `GpuKernels.cu`'s `ExpandKernel` internally, same move-gen/flip/
  canonicalize/ring-boundary-conversion logic), but writing each parent's
  children into a fixed, non-atomic, per-thread-private slot range --
  no sort, no dedup, since retrograde summing wants every (parent, child)
  edge counted once, not deduped. Both the next-player tag and a new
  color-flip tag are tracked **per child, not per parent**: canonicalization's
  16-way symmetry search includes a color-swap family, and which family
  wins is data-dependent per child, so two children of the same parent can
  differ here (caught during review; the first draft wrongly assumed one
  next-player per parent).
- **Color-flip correctness**: a child whose canonical form came from the
  color-swap symmetry has its stored black/white win counts swapped
  relative to the real, played-out continuation -- must be swapped back
  before summing into the parent (tie count unaffected). Independently
  validated against `OthelloLevelBlaster`'s own `OthelloLevelBlasterWinCalculator/
  RetroKernels.cu`, which already discovered and fixed this exact issue
  against real 4x4 data (`dev_swapColors`) -- confirms both the bug and the fix.
- New `OthelloRingMasterCalculator/TerminalClassify.h`: `ClassifyTerminalOutcome`,
  factored out of Phase 2's `TerminalLevelBootstrap.cpp` so the non-terminal
  step can classify individual terminal boards at any level the same way
  (a board can be terminal well before the deepest level).
- New `OthelloRingMasterCalculator/NonTerminalLevelStep.h`/`.cpp`:
  `ProcessNonTerminalLevel` -- the real Phase 3 driver. Loads level+1's
  nested-index lookup structures and already-computed counts (both
  colors, fully in memory -- level+1's compact board-key representation
  and finished counts array, not a wide scratch buffer), streams level's
  own boards through the GPU in batches, classifies terminal boards
  directly or sums non-terminal boards' children (with the color-flip
  swap applied), and writes results out in original board order -- no
  re-serialization step needed, since one thread per parent naturally
  preserves order. Overflow triggers an abort-and-retry of the WHOLE
  level (both colors) at the next-wider tier, per the design's abort-and-
  retry mechanism; a successful completion calls
  `CounterWidthConfigBumpLevel` to propagate this level's confirmed width
  as a floor to shallower, not-yet-processed levels.
- `OthelloRingMasterCalculator.cpp`'s `main()` now runs the Phase 2
  terminal bootstrap followed by exactly one Phase 3 non-terminal step
  (the next-shallower level), then exits -- looping this over every
  remaining level down to level 0 is Phase 4's job, not yet built.
- Known, deliberate limitation carried forward from the design
  discussion: level+1's counts array is loaded wholesale into memory for
  lookups, which is fine at the 4x4 validation scale (Phase 6) but not
  yet scale-safe for real 6x6's largest middle levels -- revisit before
  Phase 7, alongside the user's drive-sharded lookup idea already
  recorded in `project_retrograde_calculator_implementation_plan` memory.

## [0.20.0] - 2026-07-08

### Calculator Phase 2: terminal-level bootstrap -- the first real end-to-end slice

- New `OthelloRingMasterCalculator/StoreLevelScan.h`/`.cpp`:
  `FindDeepestCompleteLevel` scans RingMaster's finished store for the
  highest level with a `_complete` sentinel. Deliberately simpler than
  `InitSolver.cpp`'s own `ScanForResumeLevel` -- this calculator only ever
  reads `storeDir`, never writes to it, so there's nothing to repair or
  purge, just "stop at the first missing sentinel."
- New `OthelloRingMasterCalculator/TerminalLevelBootstrap.h`/`.cpp`:
  `ProcessTerminalLevel` reads the deepest completed level's boards (both
  black-to-move and white-to-move, via `RingNestedIndexReader`/
  `ExpandAll`), classifies each directly from final piece count
  (`std::popcount` on `ullCellsInUse`/`ullCellColors` -- more black discs
  wins, more white wins, equal ties), and streams the one-hot result out
  through `NibbleCountsWriter` from Phase 1 -- no children, no lookups,
  since every board at this level is terminal by construction. Also
  populates `CalculatorLevelStats` (per-color and combined win/tie/loss
  totals, board counts, timing, completion timestamp).
- New `OthelloRingMasterCalculator/CalculatorFileName.h`: the counts-file
  naming helper `RSFFileName.h` explicitly deferred ("a separate,
  not-yet-started future phase") -- that phase has now arrived. One
  `.counts` extension covers every tier; the reader always already knows
  the width from `CounterWidthConfig` before opening the file.
- New `OthelloRingMasterCalculator/CalculatorInitLogger.h`/`.cpp`: mirrors
  the forward solver's `InitLogger.h`/`.cpp` exactly. This closes a real
  gap Phase 0/1 left open -- `main()` never called any logger init, so
  every `LoggerLog` call anywhere in the calculator (including Phase 1's
  `CounterWidthConfigBumpLevel`) was silently discarded until now.
- `CalculatorTypes.h`: added `countsDrive`/`countsDirNameNoDrive` to
  `OthelloRingMasterCalculatorConfig` and `countsDirectory` to
  `OthelloRingMasterCalculatorState` -- the calculator's own output
  location, distinct from `storeDirectory` (RingMaster's read-only input).
  New CLI options `--counts-drive`/`--counts-dir`
  (default `Y:\OthelloRingMasterCalculator\Counts`).
- `OthelloRingMasterCalculator.cpp`'s `main()` now actually does
  something: parses args, opens the logger, finds the deepest completed
  level, and runs `ProcessTerminalLevel` against it. Phases 3+ (the real
  non-terminal backward walk, status/monitoring, 4x4 validation) remain
  unimplemented -- this processes exactly one level, once, and exits.

## [0.19.0] - 2026-07-08

### Calculator Phase 1: storage layer -- arbitrary-width counter arithmetic, streamed counts files, persistent width config

- New `Utility/WideCounter.h`/`.cpp`: generic, Othello-agnostic
  arbitrary-precision unsigned-counter addition, `WideCounterAdd`
  (byte widths 1, 2, 4, 8 natively via the CPU's own integer widths;
  9+ bytes via a manual carry-chain) and `NibbleCounterAdd` (4-bit
  counters, 0-14 usable). Overflow is detected *before* the add is
  performed for native widths (`addend > maxUsable - accum`), never by
  inspecting the result afterward -- a large addition can wrap right past
  the reserved all-ones sentinel without landing anywhere near it, so a
  post-hoc "is it all ones" check is not reliable. Bignum widths detect
  overflow via an escaped top-byte carry, plus a fallback check for
  landing on the sentinel by coincidence.
- New `OthelloRingMasterCalculator/OutcomeTriple.h`/`.cpp`: the
  domain-specific (black, white, tie) triple built on top of
  `WideCounter`, in both a `NibbleOutcomeTriple` (narrowest tier) and
  `OutcomeTriple` (byte-and-wider tiers) form. Each triple's `*Add`
  commits all three counters together or not at all, so an overflow in
  just one counter (e.g. tie) never leaves the other two half-updated.
- New `OthelloRingMasterCalculator/CalculatorCountsFile.h`/`.cpp`: the
  streamed, positionally-aligned writer/reader for one level's triples,
  riding `Lz4Stream` underneath (same bounded-memory streaming discipline
  as the forward solver's `Ring_1`/`Ring_2`/`Ring_3_4` files). Byte-and-
  wider tiers write fixed `3*byteWidth`-byte records; the nibble tier
  packs 2 boards into 3 bytes (12 bits/board x 2, zero waste), with a
  zero-padded trailing record when a level has an odd board count --
  the reader is told the true record count up front so it can discard
  that padding rather than returning it. A truncated mid-record read is
  fatal, never a silent stop.
- New `OthelloRingMasterCalculator/CounterWidthConfig.h`/`.cpp`: the
  single persistent per-board-size cache file (`counterwidthconfig_NxN.json`,
  same directory and same hand-rolled flat-JSON style as
  `Utility/DriveInfo.cpp`'s `driveinfo.json`) recording each level's
  known-good tier width across runs. `CounterWidthConfigBumpLevel`
  updates a level's width and -- since width only ever grows as level
  number decreases -- proactively raises every shallower, not-yet-
  processed level's guess that is currently narrower, logging both the
  triggering level's change and every shallower level it bumped.
- New `FATAL_COUNTER_WIDTH_CONFIG_WRITE_FAILED` in `Utility/Error.h` for
  the config-file save path.
- Not yet wired into `OthelloRingMasterCalculator.cpp`'s `main()` -- these
  are the storage-layer building blocks Phase 2 (terminal-level bootstrap)
  and later phases will drive. See `project_retrograde_calculator_implementation_plan`
  memory for the phase breakdown.

## [0.18.0] - 2026-07-09

### Add OthelloRingMasterCalculator + OthelloRingMasterCalculatorStatus (Phase 0 scaffolding)

- New project `OthelloRingMasterCalculator` -- the future retrograde
  win/tie/loss calculator, Phase 0 of the design worked through this
  session (see `project_adaptive_counter_width_design` and
  `project_retrograde_calculator_implementation_plan` memories for the
  full algorithm/format design and phase breakdown). This commit is
  scaffolding only: CLI parsing into a new `CalculatorTypes.h`
  (`OthelloRingMasterCalculatorConfig`/`State`, `WinTieLossTriple`,
  `CalculatorLevelStats`), a startup banner, no retrograde processing
  logic yet.
- New project `OthelloRingMasterCalculatorStatus` -- a standalone status
  client mirroring `OthelloRingMasterStatus` exactly (same query-on-demand
  shape, connect/ask/print, no continuous push), default port 17632 so it
  can run alongside `OthelloRingMasterStatus` (17532) and Blaster's own
  status tool (17432) without a bind conflict.
- Both new projects reuse the existing shared library structure without
  any relocation: `Utility` (`RSFWriter`/`RSFReader`, `Lz4Stream`) and
  `OthelloBasics`/`OthelloBasicsForCUDA` (`RingNestedIndexBuilder`/
  `Reader`) are already proper shared projects; `OthelloRingMasterCalculator`
  references them the same way `OthelloRingMaster` itself does.
  `RSFFileName.h` (path-naming helpers) stays where it is at the solution
  root -- header-only, `static inline` throughout, zero build output of
  its own, so an include-path reference is all that's needed, no library
  project required.
- `CalculatorTypes.h` is deliberately much smaller than `OthelloTypes.h`'s
  config/state -- the calculator has none of the forward solver's
  multi-drive/multi-writer machinery; it only ever reads one already-
  finished level at a time and writes one counts file back out.

## [0.17.0] - 2026-07-08

### Skip degenerate Ring_1/Ring_2 levels for board sizes that never use them

- The ring permutation always uses the full 8x8 geometry (4 concentric
  rings), but a smaller board's active cells are centered within the 8x8
  word and never reach the outer ring(s): 6x6 never sets any Ring_1 bit
  (the outermost 8x8 ring, row/col 0 and 7), and 4x4 never sets any Ring_1
  *or* Ring_2 bit (row/col 1 and 6 either). Storing those levels for those
  board sizes was pure overhead -- worse, simply not writing the *file*
  wouldn't have been enough on its own: the builder's group-cascade logic
  would still have emitted one wholly redundant, always-zero-pattern
  20-byte record per level per `CellsInUse` group (potentially millions of
  wasted records), not actually saved anything.
- Added `RingNestedIndexHasRing1`/`RingNestedIndexHasRing2` (`RingNestedIndex.h`)
  as the one place this board-size policy is decided. `RingNestedIndexBuilder`
  now infers which levels to skip from whether `pRing1Writer`/`pRing2Writer`
  are null (callers simply don't open a writer for a level that doesn't
  apply); `CloseRing1Group`/`CloseRing2Group` no-op for a null writer while
  still cascading down correctly, and `CellsInUseRec.offset` now points at
  whichever level is actually the next one stored (Ring_1 normally, Ring_2
  or even Ring_3_4 directly when the outer level(s) are skipped).
- `RingNestedIndexReader` mirrors this: `Load()` accepts nullable
  `ring1Path`/`ring2Path` and records which levels loaded via new
  `hasRing1`/`hasRing2` members; `ExpandAll` picks one of three walk shapes
  up front (4-level for 8x8, 3-level for 6x6, 2-level for 4x4) rather than
  branching per board.
- Added a safety net consistent with the standing never-silent-data-loss
  rule: if a supposedly-skipped level's bit pattern is ever nonzero (which
  should be geometrically impossible), `Process()` fatals immediately
  rather than silently discarding real color bits -- catches either a wrong
  board-size assumption or real data corruption, instead of masking it.
- Updated every caller (`CreateSeedFile.cpp`, `MergeFiles.cpp`'s
  `ConvertLevelOutputToNestedIndex`, `LevelSolverThread.cpp`'s
  `FeedNestedIndexLevel`/`RunGpuFeederJob`, `InitSolver.cpp`'s
  `checkLevelFile`/`deletePlayerOutputFile`) to build/pass Ring_1/Ring_2
  paths only when applicable, and `RingNestedIndexFileCount`'s "how many
  files should exist" check is now computed per board size instead of a
  hardcoded 4.

## [0.16.6] - 2026-07-07

### Remove GpuInformation.recommendedWorkerCount -- computed, never consumed

- `GpuInfo.cu` computed `recommendedWorkerCount` (2 per async engine,
  clamped [2,8]) as sizing guidance for concurrent GPU-feeder threads, but
  `InitSolver.cpp` hardcodes `numGPUFeederThreads = 1` and never reads it --
  this project creates exactly one `GpuAccumulator` per level (one thread
  ever needs sizing), so the field implied a flexibility nothing in this
  design uses. Removed the field, its computation, and its log line.
- Added a comment at `InitSolver.cpp`'s `numGPUFeederThreads = 1` explaining
  why: if a future design ever needs more than one feeder thread, that's a
  decision this project makes from its own requirements, not something a
  generic GPU capability query should hand it a number for.

## [0.16.5] - 2026-07-07

### Never silently drop data on a capacity/read failure -- fatal loudly instead

Prompted by an explicit standing rule from the user: if the solver ever
misses or drops real data, it must fatal with as much diagnostic detail as
possible, never continue silently. Audited every place a "we expected to
find/read something and didn't" condition existed and was previously either
silently truncated or logged as a mere warning:

- **`DoEndOfLevelMerge` Phase 1** (added in 0.16.4's counting rework) --
  added a `Fatal(FATAL_MERGE_LOGIC_ERROR, ...)` safety net if real
  enumeration ever somehow reaches the counted capacity, since that should
  now be structurally impossible; hitting it means a real invariant was
  violated.
- **`DoCrossDriveIntermediateMerge`** -- both its writer-file gather (whose
  true count is exactly known ahead of time from consumed/snapshot index
  deltas) and its total-flush imerge gather now fatal if they hit their
  respective capacities, instead of the old silent truncation.
- **`RingNestedIndexReader::Load` callers in `LevelSolverThread.cpp`** --
  `Load()` returning `false` was being treated as one thing ("no data for
  this level/player, skip"), but it actually conflates two very different
  cases: files genuinely absent (expected, fine) vs. files present but
  corrupt/truncated (a real problem). Added `RingNestedIndexFileCount`
  (`OthelloBasics/RingNestedIndex.h`/`.cpp`) to tell them apart: 0 files
  found is still a silent skip, but 1+ files found with `Load()` still
  failing is now a `Fatal` naming all four file paths and the exact level/
  player. Also used to simplify `InitSolver.cpp`'s `checkLevelFile`, which
  already had its own equivalent counting loop.
- **`KWayMergeFiles`** -- a file that failed to open was logged as a mere
  `"WARNING skipping unreadable file"` and silently excluded from the
  merge. Every file passed in was just enumerated as present moments
  earlier by the caller, and nothing can be deleting files at this point in
  the pipeline, so an open failure here means real corruption, not a
  race -- now a `Fatal` naming the exact path.

## [0.16.4] - 2026-07-07

### Replace guessed end-of-level merge buffer sizing with an exact count

- `DoEndOfLevelMerge`'s Phase 1 file enumeration allocated its path/size
  arrays using a fixed guess, `MAX_MERGE_FANIN * MAX_MERGE_FANIN` (12.25M
  entries, ~392MB) -- inherited verbatim from the sibling production
  solution's own code, never actually checked against real numbers. The
  largest real production run on record needed only ~42,000 files for one
  color (12 cascade groups at `MAX_MERGE_FANIN`), ~291x less than the guess.
  Guessing wrong in the other direction is a real risk: if the true file
  count ever exceeded the guess, `EnumerateByPattern`'s capacity checks
  would silently stop collecting more files -- real solved board data
  quietly dropped from the merge, not an error or a crash.
- `DoEndOfLevelMerge` only ever runs after both `WaitForPoolIdle` calls and
  `FlushAllMergeWriterBuffers` complete (see `OthelloRingMaster.cpp`'s main
  loop) -- no thread can still be creating writer/imerge files by the time
  Phase 1 enumerates them, so the on-disk file set is provably static at
  that point. That makes a count-then-allocate approach safe: added
  `CountByPattern` (a `FindFirstFileA`/`FindNextFileA` walk that only
  counts, mirroring `EnumerateByPattern`'s own walk) and
  `CountEndOfLevelInputFiles` (runs the exact same pattern set Phase 1
  checks, just counting instead of collecting). Phase 1 now counts first,
  then allocates exactly that count plus a small fixed pad (256) --
  correct at any board size or data volume, no guessing, no wasted memory.
- Added a loud `Fatal(FATAL_MERGE_LOGIC_ERROR, ...)` safety net if the real
  enumeration ever somehow reaches the counted capacity anyway -- since
  that should be structurally impossible given the above, hitting it means
  a real invariant violation happened, and continuing would silently merge
  an incomplete file set.
- Left `DoCrossDriveIntermediateMerge`'s similarly-shaped `kMaxFiles`
  bound alone -- it runs while other merge-writer threads may still be
  actively producing files, so a naive count-then-allocate isn't safe
  there without more care, and its existing bound already has ~2.5x
  headroom over the same real 42,000-file high-water mark.

## [0.16.3] - 2026-07-07

### Fix --board-size CLI validation to match what's actually supported

- `--board-size` accepted any value 2..12, but `GetMaxMovesForBoardSize`
  and `BoardKeyAllocateFirstBoard` only ever handle 4/6/8 -- anything else
  (5, 7, 9, 10, 11, 12, or the endpoints 2/3) passed CLI validation and
  then failed deep in the pipeline (`CreateSeedFile.cpp`) with a
  misleading `FATAL_ALLOCATION_FAILED` instead of a clear error at the
  CLI boundary. Narrowed validation to exactly 4, 6, or 8, and corrected
  the usage text (previously read "e.g. 4 for 4x4, 6 for 6x6", not
  mentioning 8 or that only those three sizes are valid).

## [0.16.2] - 2026-07-07

### Collapse writerDriveStats drive-letter lookups to direct indexing

- Follow-up to v0.16.1: once `writerDriveStats[i]` is guaranteed built
  1:1 with `mwDirectory[i]` (one merge-writer directory per drive, no
  more "multiple dirs per drive" support), the three remaining
  search-by-drive-letter loops that assumed a possible many-to-one
  relationship became provably dead work -- each could only ever match
  index `i` itself.
- `MergeFiles.cpp`'s post-flush merge-trigger check and
  `DoCrossDriveIntermediateMerge`'s under-lock space recheck both now
  index `writerDriveStats[ti]`/`writerDriveStats[i]` directly instead of
  scanning for a matching `driveLetter`.
- `StatsListener.cpp`'s live status display now reads
  `mwBlackFileCount[i]`/`mwWhiteFileCount[i]` directly instead of
  scanning every merge-writer thread for one whose directory sits on the
  current drive.
- No behavior change -- these loops always found exactly one match at
  the same index; this just removes the now-unnecessary search.

## [0.16.1] - 2026-07-07

### Remove leftover "multiple merge-writer directories per drive" support

- Caught by the user as dead architecture carried over from a long time ago:
  `createMergeWriterDirectoryName` took a `dirNumber` parameter "reserved for
  multiple dirs per drive" but was only ever called with `0`, and
  `WriterDriveStats.numDirs` existed to count how many merge-writer
  directories shared one physical drive -- but the construction loop only
  ever creates one merge-writer directory per unique fast drive, so
  `numDirs` was always exactly `1` and the per-drive-letter grouping logic
  that built it never actually collapsed anything.
- Removed `numDirs` from `WriterDriveStats` (`OthelloTypes.h`) and the
  `DRIVE_SPACE_LOW_BYTES * numDirs` multiplication in `InitSolver.cpp`
  (threshold is now just `DRIVE_SPACE_LOW_BYTES`, its actual value in every
  real run). Simplified the per-drive-stats construction loop to a direct
  1:1 init (`writerDriveStats[i]` <-> `mwDirectory[i]`), removing a
  search-by-drive-letter loop that could never find a match given each
  drive only ever gets one entry.
- Removed the now-meaningless `dirNumber` parameter from
  `createMergeWriterDirectoryName`, and the `_0` suffix it produced in the
  on-disk directory name (`writerDir_0` -> `writerDir`, matching
  `mergeDir`'s own no-suffix naming). These directories are ephemeral
  (wiped and recreated every startup by `cleanUpDrives`), so no
  resume-compatibility concern.
- Removed the `Dirs` column from `StatsListener.cpp`'s live status
  drive-breakdown table (it always printed `1`).

## [0.16.0] - 2026-07-07

### Make the nested-index compression actually streaming, fix the resume scan, and clean up stale comments

- **Closes the compression gap flagged right after v0.15.0/v0.15.1**:
  `RingNestedIndexBuilder` was writing `Ring_1`/`Ring_2`/`Ring_3_4` fully
  uncompressed, and `CellsInUse` through a compression tier that never
  actually engaged -- meaning the nested-index format could plausibly land
  larger on disk than the flat format it replaced, undermining the point of
  the whole ring-ordering effort.
- Added `Utility/Lz4Stream.h`/`.cpp`: a generic streaming LZ4-frame
  compressor/decompressor (`Lz4StreamWriter`/`Lz4StreamReader`) for byte
  streams that don't fit `RingStoreFile.h`'s two-`uint64_t`-field shape.
  Buffers up to 256KB before compressing/flushing a chunk (write side) or
  decompressing on demand (read side) -- bounded memory regardless of
  total stream length, with no raw intermediate file ever written. `Ring_1`/
  `Ring_2`/`Ring_3_4` now go through this.
- `CellsInUse`'s `(pattern, offset)` shape is bit-identical to
  `Utility/RingStoreFile.h`'s `UINT64_PAIR`, so it goes through the
  existing `RSFWriter`/`RSFReader` machinery instead of a second bespoke
  format -- but doing so exposed a second bug: `RSFWriterOpenZ` only turns
  on its LZ4 layer when the path contains `.rsfzl`, and `CellsInUse`'s
  fixed `.cellsinuse` extension never matches that. Added
  `RSFWriterOpenZL` (`Utility/RingStoreFile.h`/`.cpp`): forces the LZ4
  layer on regardless of the path's extension, for callers like this one
  whose naming convention doesn't -- and can't -- use `.rsfzl`.
- `RingNestedIndexBuilder::Init`'s signature changed from four raw `FILE*`
  to `(RSFWriter*, Lz4StreamWriter*, Lz4StreamWriter*, Lz4StreamWriter*)`;
  updated all three call sites (`MergeFiles.cpp`'s
  `ConvertLevelOutputToNestedIndex`, `CreateSeedFile.cpp`) to open/close
  through the new writer types. `LevelSolverThread.cpp`'s read side needed
  no change -- it only calls `RingNestedIndexReader::Load`, whose external
  signature was already unchanged.
- **Fixed a real resume-scan bug**: `InitSolver.cpp`'s `checkLevelFile`/
  `deletePlayerOutputFile` only ever looked for the legacy flat
  `.rsf`/`.rsfz`/`.rsfzl` extensions -- meaning every level completed since
  v0.15.0 (which are all written as the 4-file nested index) would show up
  as absent on restart, forcing a full re-solve from that level every time.
  Both functions now check the nested-index four-file set first
  (validating via `RingNestedIndexReader::Load`), falling back to the
  legacy flat check for stores produced before the nested-index format
  existed.
- Removed every remaining live-source-comment reference naming deleted or
  external files/tools by name (an offline ring-split validation tool, an
  older on-disk record format, and an unrelated external consumer that
  briefly showed up in one file's Notes) -- replaced with generic
  descriptions ("an earlier solver implementation", "an earlier offline
  analysis tool") so nothing in this codebase sends a future reader
  looking for something that isn't here. `CHANGELOG.md` (historical) and
  `README.md`'s still-accurate reference to the sibling analysis tool are
  unaffected.
- C-style verification pass on `OthelloBasics`/`OthelloBasicsForCUDA`
  (flagged since Phase 0, never done until now): fixed three off-by-one
  field/constant alignment bugs (`RingNestedIndex.h`'s
  `RingNestedIndexStats` and `RingNestedIndexBuilder` fields,
  `RingConversion.cu`'s starting-position constants, and a wrapped-comment
  continuation in `OthelloBasicsForCUDA.h`'s `BOARD` struct), and added
  the missing file banner/Doxygen header to `OthelloBasicsForCUDA.cu`
  (previously just two bare lines with no documentation at all).

## [0.15.1] - 2026-07-07

### Fix v0.15.0 build: forward-declare FlushAccumulator

- `FeedBoardIntoBatch`/`FeedNestedIndexLevel` landed in the file slot
  previously occupied by `EnumerateStoreFilesForLevel` (before
  `FlushAccumulator`'s own definition further down), but unlike that old
  function, they call `FlushAccumulator` -- caught by the user's build as
  `C3861: 'FlushAccumulator': identifier not found`. Added a forward
  declaration in `LevelSolverThread.cpp` immediately before
  `FeedBoardIntoBatch`.

## [0.15.0] - 2026-07-07

### Wire the ring nested-index into the live store write/read path

- **Closes the gap flagged right after Phase 4 completed**: `DoEndOfLevelMerge`
  previously wrote each level as a flat, sorted+deduped RSF file (ring-ordered
  bit content, but the same one-file-per-player shape Blaster always used) --
  the `RingNestedIndexBuilder`/`Reader` machinery from Phase 3 was never
  actually attached to the live pipeline, so none of the validated extra
  storage savings (2.4-2.9% compressed / 5.77x uncompressed, see
  project_ring_split_validated_findings memory) were being realized.
- Added `ConvertLevelOutputToNestedIndex` (`MergeFiles.cpp`, new, not ported
  from Blaster): after `DoEndOfLevelMerge`'s merge produces the flat sorted
  output, re-reads it and rebuilds it as the 4-file `CellsInUse`/`Ring_1`/
  `Ring_2`/`Ring_3_4` nested index, deleting the flat intermediate only
  after every nested file is fully written and closed -- so an interruption
  mid-conversion just falls back to the existing "re-solve the level from
  scratch" resume behavior, no new failure mode.
- Added `RSFNameCellsInUseFile`/`RSFNameRing1File`/`RSFNameRing2File`/
  `RSFNameRing34File` (`RSFFileName.h`) for the new `.cellsinuse`/`.ring1`/
  `.ring2`/`.ring34` file naming.
- Replaced `LevelSolverThread.cpp`'s read side: `EnumerateStoreFilesForLevel`
  + `RSFOpen`/`RSFRead` (dead now, removed) became `FeedNestedIndexLevel`/
  `FeedBoardIntoBatch`, which `RingNestedIndexReader::Load`s a level's 4
  index files and `ExpandAll`s them back into the same ping-pong-buffered
  GPU-batch feed the old flat-file loop did.
- `CreateSeedFile.cpp` now writes level 0's single starting board in the
  same nested-index format, so the very first level the feeder ever reads
  is already in the steady-state format.
- **Known follow-up, not fixed now**: `InitSolver.cpp`'s `checkLevelFile`/
  `deletePlayerOutputFile` resume-scan fallback (used only when a level has
  no sentinel at all) still only knows the old flat `.rsf`/`.rsfz`/`.rsfzl`
  extensions. Harmless in practice -- every level in this project now always
  gets a sentinel written -- but worth revisiting if that ever stops being true.

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
