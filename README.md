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

Early stage. So far:
- Solution scaffolded; `Utility` subproject ported over from
  `OthelloLevelBlaster` (general-purpose infra: memory arena, binary search,
  clock/timing, drive detection+benchmarking, error handling, file/dir
  helpers, logging, tracked heap allocation, reader/writer locking, thread
  pool).
- The ring-split storage layout, GPU boundary conversion, and retrograde
  counter work described above are designed (see project history / internal
  notes) but not yet implemented in this solution.

## Related

`OthelloLevelBlaster` (sibling solution, not touched by this project) is the
source of the design work and reusable infrastructure behind this project,
and remains the actively-running production solver.
