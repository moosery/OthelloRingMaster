# OthelloRingMaster

A searchable, GPU-native, ring-ordered board+outcome database for Othello --
informally a "tablebase" (borrowing the term from solved-game chess/checkers
endgame databases).

This is a fresh, isolated solution, separate from the sibling
`OthelloLevelBlaster` project (a precious, actively-running production
solver). The design work behind this project happened inside
`OthelloLevelBlaster` first, as an offline analysis tool
(`OthelloRingSplitAnalyzer`); this solution is where that design becomes
real, since it needs to touch the live GPU expansion/dedup pipeline rather
than just read an already-finished store.

## The mission

1. **Store boards using a ring-gathered bit layout instead of row-major**,
   split into a nested index (`CellsInUse` -> `Ring_1` -> `Ring_2` ->
   `Ring_3_4`). Validated on real 6x6 production data: ~2.4-2.9% smaller
   compressed, 5.77x smaller uncompressed -- the uncompressed number is what
   matters for point-lookup use cases, since compression breaks random-access
   seeking.

   `Ring_3_4` is deliberately kept as one combined 16-bit leaf, not split
   further into a `Ring_3` grouping level + `Ring_4` leaf. Tested directly on
   real 6x6 data (levels 15 and 17): splitting made total storage ~5-6%
   *worse* both times, not better. The existing delta+varint+LZ4 compression
   already captures real structure in the combined 16-bit value (real cost
   came in around 6.9 bits/board, well under the raw 16), and a `Ring_3`
   grouping level's own per-group offset overhead consistently outweighed
   what little additional entropy a split could recover -- even accounting
   for `Ring_4` alone showing some real skew (nibble-packed + LZ4 landed
   under the naive 4 bits/board), it wasn't enough to offset `Ring_3`'s cost.
2. **Keep the live GPU math (move generation, flip computation,
   canonicalization) entirely row-major** -- ring order is fundamentally
   incompatible with the uniform-shift trick that math depends on. Convert
   bits only at the GPU's boundary: ring -> row-major when loading a level's
   stored boards in, row-major -> ring when handing deduped output out.
3. **Compute and store per-board win/tie/loss retrograde counts**, in a
   separate file positionally aligned to `Ring_3_4`, with adaptive per-level
   counter width (nibble/byte/short/int/.../128-bit) discovered at runtime
   rather than pre-guessed. This is what finally makes a best-move oracle
   work on 6x6, which the current tooling cannot do at all.

## Status

Core pipeline implemented and validated on 4x4:
- **`OthelloRingMaster`** -- the forward solver. Ring-gathered nested-index
  storage (`CellsInUse` -> `Ring_1` -> `Ring_2` -> `Ring_3_4`), row-major GPU
  move-generation/dedup with ring<->row-major boundary conversion,
  drive-spanning multi-writer/merge pipeline, whole-level resumability via
  sentinel files, live TCP status querying (`OthelloRingMasterStatus`).
- **`OthelloRingMasterCalculator`** -- the retrograde win/tie/loss
  calculator. Walks a completed `OthelloRingMaster` store backward from its
  deepest level to level 0: classifies terminal boards directly, sums
  non-terminal ones by regenerating children on the GPU and looking each
  one up against level+1's already-computed results (via drive-spanning
  segmented scratch, never holding a whole level resident in memory), with
  adaptive per-level counter width (nibble through arbitrary byte widths,
  widening automatically on overflow). Whole-level resumability, live TCP
  status querying (`OthelloRingMasterCalculatorStatus`), table-ized
  per-level logging, `--force` to force a clean recompute.
- **Validated end to end on 4x4**: a full `OthelloRingMaster` +
  `OthelloRingMasterCalculator` run reproduces the known-correct whole-tree
  result exactly -- 24,632 black wins / 30,116 white wins / 5,312 ties.
- Not yet run at 6x6 or 8x8 scale.

## Related

`OthelloLevelBlaster` (sibling solution, not touched by this project) is the
source of the design work and reusable infrastructure behind this project,
and remains the actively-running production solver.
