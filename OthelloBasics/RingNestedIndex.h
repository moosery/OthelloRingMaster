/*
** Filename:  RingNestedIndex.h
**
** Purpose:
**   Declares the ring-split nested-index format (CellsInUse -> Ring_1 ->
**   Ring_2 -> Ring_3_4), promoted out of the offline analysis tool that
**   originally validated this design (see project_ring_split_validated_
**   findings memory): RingNestedIndexBuilder consumes a sorted, deduped
**   stream of ring-ordered BOARD_KEYs and writes the nested files (four for
**   8x8; fewer for smaller boards, see below); RingNestedIndexReader does
**   the reverse, expanding the files back into the original sorted stream
**   of BOARD_KEYs.
**
**   This is CPU-organizing work, not solving -- pure counting/comparison/
**   offset bookkeeping over already-ring-ordered numeric keys, per the
**   CPU-organizes/GPU-solves boundary. It never touches row-major bit
**   structure.
**
** Notes:
**   The 64-bit color pattern is always split as 28/20/16 bits (Ring_1/
**   Ring_2/Ring_3_4, with CellsInUse carrying the full 64-bit occupancy
**   pattern separately) -- validated on real production data, see
**   project_ring_split_validated_findings memory. Ring_3_4 groups are
**   always exactly 1 member (pattern + Ring_1 + Ring_2 + Ring_3_4 together
**   reconstruct one board exactly, and the source store has no
**   duplicates), which is why Ring34Rec carries no count field.
**
**   Ring_1/Ring_2 are skipped entirely for board sizes that provably never
**   set any bit in them (see RingNestedIndexHasRing1/HasRing2 below): 6x6
**   never touches Ring_1, and 4x4 never touches Ring_1 or Ring_2, since a
**   smaller board's active cells are centered within the 8x8 word and
**   never reach the outer ring(s). Skipping them isn't just "don't write
**   the file" -- without it, the builder would still emit one wholly
**   redundant, always-zero-pattern record per level per CellsInUse group
**   (pure overhead, not a size optimization at all). `RingNestedIndexBuilder`
**   detects which levels to skip from whether `pRing1Writer`/`pRing2Writer`
**   are null; `RingNestedIndexReader` from whether `ring1Path`/`ring2Path`
**   passed to `Load()` are null. Callers decide via
**   `RingNestedIndexHasRing1`/`RingNestedIndexHasRing2`.
**
**   All four files share the same delta+varint+LZ4 (.rsfzl) compression,
**   via Utility/RingStoreFile.h's shaped writer/reader entry points
**   (RSFWriterOpenZLShaped/RSFWriterRecordShaped/RSFOpenShaped/
**   RSFReadShaped): CellsInUse's (pattern, offset) shape is bit-identical
**   to RSF_SHAPE_PAIR64 (the plain UINT64_PAIR API also works for it, and
**   is what's used); Ring_1/Ring_2 use RSF_SHAPE_RING_LEVEL (u32 pattern +
**   u64 offset -- no count field, same reasoning as Ring_3_4 below);
**   Ring_3_4 uses RSF_SHAPE_LEAF16. Nothing here holds a whole level in
**   memory during a Process()/StreamAll() pass -- this matters at real
**   data volumes.
**
**   Ring_1/Ring_2 carry no count field, mirroring CellsInUse: a group's
**   span in the next level down is implied by the next record's offset
**   (or "consume until that level's stream hits EOF" for the very last
**   record in the WHOLE flat Ring_1/Ring_2 stream -- offsets are written
**   monotonically non-decreasing across the entire level, not just within
**   one parent group, so this lookahead is correct regardless of which
**   parent group a record belongs to). Storing count explicitly would cost
**   real disk space for zero benefit: nothing in this pipeline ever binary
**   searches directly against a compressed ring file -- the retrograde
**   calculator's binary search always runs against its own decompressed
**   scratch copy, built via one unavoidable sequential pass regardless.
*/

#pragma once

/* Includes */
#include "OthelloBasics.h"
#include "RingStoreFile.h"
#include <cstdint>
#include <functional>
#include <vector>

/* Constants */

/* The 64-bit color pattern is partitioned with no gaps/overlaps: bits
** [RING1_SHIFT..63] are Ring_1's subpattern, [RING2_SHIFT..RING1_SHIFT-1]
** are Ring_2's, [0..RING2_SHIFT-1] are Ring_3_4's.
*/
constexpr int RING_TOTAL_BITS = 64;
constexpr int RING1_BITS      = 28;
constexpr int RING2_BITS      = 20;
constexpr int RING34_BITS     = 16;
constexpr int RING1_SHIFT     = RING_TOTAL_BITS - RING1_BITS;                 /* 36 */
constexpr int RING2_SHIFT     = RING_TOTAL_BITS - RING1_BITS - RING2_BITS;    /* 16 */
constexpr int RING34_SHIFT    = 0;

/* Functions */

/*
** Function: RingNestedIndexHasRing1
** @brief    True if boardSize's board data can ever set a Ring_1 bit.
** @details  Ring_1 is exactly the outermost ring of the full 8x8 ring
**           geometry (row/col 0 and 7) -- a smaller board's active cells,
**           centered within the 8x8 word, never reach that ring. Only
**           8x8 itself ever needs a Ring_1 level; 4x4/6x6 boards would
**           store a Ring_1 file that's provably always one degenerate
**           all-zero group, so this project skips it entirely for them.
** @param    boardSize - board size (4, 6, or 8)
** @return   true only for boardSize == 8.
*/
inline bool RingNestedIndexHasRing1(int boardSize) { return boardSize >= 8; }

/*
** Function: RingNestedIndexHasRing2
** @brief    True if boardSize's board data can ever set a Ring_2 bit.
** @details  Ring_2 is the second ring in from the 8x8 border (row/col 1
**           and 6) -- a 4x4 board's active cells, centered within the 8x8
**           word, never reach that ring either (only 6x6 and 8x8 do).
** @param    boardSize - board size (4, 6, or 8)
** @return   true for boardSize == 6 or 8; false for 4x4.
*/
inline bool RingNestedIndexHasRing2(int boardSize) { return boardSize >= 6; }

/* Structures and Types */

/*
** Type:    CellsInUseRec
** @brief   One entry per distinct ring-gathered occupancy pattern: the
**          pattern itself, plus the offset into whichever level is the
**          next one actually stored for this board size (Ring_1 normally;
**          Ring_2 or even Ring_3_4 directly when the outer ring level(s)
**          are skipped -- see RingNestedIndexHasRing1/HasRing2). No count
**          field -- the span is implied by the next entry's offset (or the
**          end of that level's array, for the last entry).
*/
#pragma pack(push, 1)
struct CellsInUseRec
{
    uint64_t pattern;   /* ring-gathered occupancy (BOARD_KEY::ullCellsInUse in ring order) */
    uint64_t offset;    /* index into the next stored level's array (see struct comment)     */
};

/*
** Type:    RingLevelRec
** @brief   Shared record shape for both Ring_1 and Ring_2: this level's
**          color subpattern, and the offset into the next level down where
**          this group's records start. No count field -- the span is
**          implied by the next record's offset in this same flat stream
**          (or "consume until EOF" for the last one), exactly like
**          CellsInUseRec -- see file Notes.
*/
struct RingLevelRec
{
    uint32_t pattern;   /* low RING1_BITS or RING2_BITS meaningful, rest always 0            */
    uint64_t offset;    /* index into the next level down where this group's records start  */
};

/*
** Type:    Ring34Rec
** @brief   One entry per board: the combined mid+inner-ring color
**          subpattern. No count field -- every group has exactly 1 member
**          (see file Notes), so storing a count would be pure dead weight.
*/
struct Ring34Rec
{
    uint16_t pattern;   /* low RING34_BITS meaningful (mid+inner ring combined) */
};
#pragma pack(pop)
static_assert(sizeof(CellsInUseRec) == 16, "CellsInUseRec must be 16 bytes");
static_assert(sizeof(RingLevelRec)  == 12, "RingLevelRec must be 12 bytes");
static_assert(sizeof(Ring34Rec)     ==  2, "Ring34Rec must be 2 bytes");

/*
** Type:    RingNestedIndexStats
** @brief   Record counts at each level, plus a sanity counter for the
**          "every Ring_3_4 group has exactly 1 member" invariant -- a
**          violation shows up here instead of silently losing data.
*/
struct RingNestedIndexStats
{
    uint64_t cellsInUseRecords         = 0;
    uint64_t ring1Records              = 0;
    uint64_t ring2Records              = 0;
    uint64_t ring34Records             = 0;
    uint64_t ring34GroupsWithCountNot1 = 0;
    uint64_t totalBoards               = 0;
};

/*
** Type:    RingNestedIndexBuilder
** @brief   Consumes a sorted, deduped stream of ring-ordered BOARD_KEYs
**          (via repeated Process() calls) and writes the nested index
**          files (four for 8x8, fewer for smaller boards -- see file
**          Notes). Call Finish() once after the last Process() call to
**          flush any still-open groups.
*/
struct RingNestedIndexBuilder
{
    RSFWriter*  pCellsInUseWriter = nullptr;   /* RSF_SHAPE_PAIR64, opened via RSFWriterOpenZL      */
    RSFWriter*  pRing1Writer      = nullptr;   /* RSF_SHAPE_RING_LEVEL, opened via RSFWriterOpenZLShaped */
    RSFWriter*  pRing2Writer      = nullptr;   /* RSF_SHAPE_RING_LEVEL, opened via RSFWriterOpenZLShaped */
    RSFWriter*  pRing34Writer     = nullptr;   /* RSF_SHAPE_LEAF16, opened via RSFWriterOpenZLShaped      */

    bool      havePattern            = false;
    uint64_t  curPattern             = 0;

    bool      haveRing1Group         = false;
    uint32_t  curRing1Pattern        = 0;
    uint64_t  ring1GroupRing2Start   = 0;

    bool      haveRing2Group         = false;
    uint32_t  curRing2Pattern        = 0;
    uint64_t  ring2GroupRing34Start  = 0;

    bool      haveRing34Group        = false;
    uint16_t  curRing34Pattern       = 0;
    uint64_t  ring34GroupCount       = 0;

    RingNestedIndexStats stats;

    /*
    ** Method: Init
    ** @brief  Attaches the already-open outputs this builder writes to.
    ** @param  pCellsInUseWriterIn - already-open RSFWriter for the CellsInUse output (via RSFWriterOpenZL)
    ** @param  pRing1WriterIn      - already-open RSFWriter (RSF_SHAPE_RING_LEVEL, via RSFWriterOpenZLShaped) for the Ring_1 output, or nullptr if
    **                               RingNestedIndexHasRing1(boardSize) is false (that level isn't stored)
    ** @param  pRing2WriterIn      - already-open RSFWriter (RSF_SHAPE_RING_LEVEL, via RSFWriterOpenZLShaped) for the Ring_2 output, or nullptr if
    **                               RingNestedIndexHasRing2(boardSize) is false (that level isn't stored)
    ** @param  pRing34WriterIn     - already-open RSFWriter (RSF_SHAPE_LEAF16, via RSFWriterOpenZLShaped) for the Ring_3_4 output (always required)
    */
    void Init(RSFWriter* pCellsInUseWriterIn, RSFWriter* pRing1WriterIn,
              RSFWriter* pRing2WriterIn, RSFWriter* pRing34WriterIn);

    /*
    ** Method: Process
    ** @brief  Feeds one ring-ordered BOARD_KEY into the builder. Keys must
    **         arrive already sorted (ullCellsInUse then ullCellColors) and
    **         deduped -- the same order RSFOpen/RSFRead's stream is already in.
    ** @param  key - the next ring-ordered BOARD_KEY in sorted order
    */
    void Process(const BOARD_KEY& key);

    /*
    ** Method: Finish
    ** @brief  Flushes any still-open groups. Call once after the last Process().
    */
    void Finish();

private:
    void CloseRing34Group();
    void CloseRing2Group();
    void CloseRing1Group();
};

/*
** Type:    RingNestedIndexReader
** @brief   Loads the nested index files into memory and expands them back
**          into the original sorted stream of ring-ordered BOARD_KEYs.
*/
struct RingNestedIndexReader
{
    std::vector<CellsInUseRec>  cellsInUse;
    std::vector<RingLevelRec>   ring1;
    std::vector<RingLevelRec>   ring2;
    std::vector<Ring34Rec>      ring34;

    bool  hasRing1 = false;   /* set by Load(); false if ring1Path was nullptr */
    bool  hasRing2 = false;   /* set by Load(); false if ring2Path was nullptr */

    /*
    ** Method: Load
    ** @brief  Reads the nested index files fully into memory.
    ** @param  cellsInUsePath - path to the CellsInUse file
    ** @param  ring1Path      - path to the Ring_1 file, or nullptr if
    **                          RingNestedIndexHasRing1(boardSize) is false
    ** @param  ring2Path      - path to the Ring_2 file, or nullptr if
    **                          RingNestedIndexHasRing2(boardSize) is false
    ** @param  ring34Path     - path to the Ring_3_4 file (always required)
    ** @return true if every applicable file was read successfully.
    */
    bool Load(const char* cellsInUsePath, const char* ring1Path, const char* ring2Path, const char* ring34Path);

    /*
    ** Method: GetBoardCount
    ** @brief  Returns the total number of boards represented by the loaded index.
    ** @return ring34.size() (one entry per board -- see file Notes).
    */
    uint64_t GetBoardCount() const;

    /*
    ** Method: ExpandAll
    ** @brief  Walks the nested index and calls onBoard once per board, in
    **         the same sorted order the index was built from.
    ** @param  onBoard - called once per board with the reconstructed ring-ordered BOARD_KEY
    */
    void ExpandAll(const std::function<void(const BOARD_KEY& key)>& onBoard) const;

    /*
    ** Method: FindBoardPosition
    ** @brief  Finds key's ordinal position among this level's boards -- the
    **         same 0-based index ExpandAll would have delivered it at --
    **         via a binary search at each level of the hierarchy instead of
    **         a full ExpandAll walk. Used by the retrograde calculator to
    **         locate a generated child's already-computed record in the
    **         next level's counts file (positionally aligned to this same
    **         board order -- see project_adaptive_counter_width_design memory).
    ** @param  key          - the ring-ordered BOARD_KEY to find
    ** @param  pOutPosition - out: key's 0-based position (== index into ring34), if found
    ** @return true if key was found in this level's store.
    */
    bool FindBoardPosition(const BOARD_KEY& key, uint64_t* pOutPosition) const;
};

/*
** Function: RingNestedIndexStreamAll
** @brief    Streams every board directly from the four nested-index files
**           on disk, in original sorted order, calling onBoard once per
**           board -- never holding a whole level resident, regardless of
**           board count (unlike RingNestedIndexReader::Load()/ExpandAll(),
**           which load all four files into memory first).
** @details  Reads CellsInUse, Ring_1/Ring_2 (if applicable) and Ring_3_4
**           (all via RSFReader/RSFReaderShaped) in lockstep, one record at
**           a time from each. Every non-leaf level (CellsInUse, Ring_1,
**           Ring_2) needs a one-record lookahead to know where its current
**           group ends -- none of them carry an explicit child count (see
**           file Notes) -- and each level's very last group's span is
**           simply "keep consuming from the next stored level until it
**           hits EOF," which needs no upfront total count either. Total
**           memory held at any moment is a small, fixed handful of
**           records (2 each of CellsInUse/Ring_1/Ring_2 that apply, plus 1
**           Ring_3_4), never proportional to the level's board count.
** @param    cellsInUsePath - path to the CellsInUse file
** @param    ring1Path      - path to the Ring_1 file, or nullptr if not applicable
** @param    ring2Path      - path to the Ring_2 file, or nullptr if not applicable
** @param    ring34Path     - path to the Ring_3_4 file (always required)
** @param    onBoard        - called once per board with the reconstructed ring-ordered BOARD_KEY
** @return   true if every applicable file opened and was read to a clean
**           completion (no truncation/corruption partway through).
*/
bool RingNestedIndexStreamAll(const char* cellsInUsePath, const char* ring1Path, const char* ring2Path,
                              const char* ring34Path, const std::function<void(const BOARD_KEY& key)>& onBoard);

/*
** Type:    RingNestedIndexPullReader
** @brief   Pull-style counterpart to RingNestedIndexStreamAll -- Peek()/
**          Advance() give one board at a time on demand instead of a
**          callback run to completion, so a k-way merge heap can
**          interleave several of these (one per merge input) alongside
**          each other. Needed once merge inputs are themselves ring-
**          format (cascade's own intermediate/grouped temp files, once
**          those become ring-format too) -- StreamAll's callback shape
**          can't participate in a heap that pops one record at a time
**          across many sources.
** @details Internally mirrors RingNestedIndexBuilder's own cascading
**          CloseRing1Group/CloseRing2Group/CloseRing34Group logic, but in
**          reverse: instead of counters that trigger a WRITE when a group
**          closes, running consumed-record counts per level (ring1Count/
**          ring2Count/ring34Count) trigger advancing that level's READ
**          cursor once the count reaches the next record's offset (the
**          same boundary the builder itself wrote) -- a cascading carry,
**          like an odometer. The one-record lookahead on CellsInUse/
**          Ring_1/Ring_2 plus an ADDITIONAL one on Ring_3_4 (needed here,
**          unlike StreamAll, so Peek() can report the current board without
**          having consumed it) are the only records ever held in memory.
**          Same O(1)-memory guarantee as StreamAll, regardless of board count.
*/
struct RingNestedIndexPullReader
{
    RSFReader* pCellsInUse = nullptr;
    RSFReader* pRing1      = nullptr;
    RSFReader* pRing2      = nullptr;
    RSFReader* pRing34     = nullptr;
    bool       hasRing1    = false;
    bool       hasRing2    = false;

    UINT64_PAIR   curCells{}, nextCells{};
    bool          haveCurCells = false, haveNextCells = false;

    RingLevelRec  curRing1{}, nextRing1{};
    bool          haveCurRing1 = false, haveNextRing1 = false;

    RingLevelRec  curRing2{}, nextRing2{};
    bool          haveCurRing2 = false, haveNextRing2 = false;

    Ring34Rec     curRing34{};
    bool          haveCurRing34 = false;

    /* Running counts of records consumed so far from each level -- the
    ** "position" half of the odometer-style carry logic described above.
    */
    uint64_t ring1Count  = 0;
    uint64_t ring2Count  = 0;
    uint64_t ring34Count = 0;

    bool      haveCurrent = false;   /* true if currentBoard is a real, not-yet-consumed board */
    bool      corrupted   = false;   /* true if an expected-more-data boundary hit EOF early -- see Advance() */
    BOARD_KEY currentBoard{};

    /*
    ** Method: Open
    ** @brief  Opens the applicable nested-index files and primes the
    **         lookahead so Peek() immediately reports the first board.
    ** @param  cellsInUsePath - path to the CellsInUse file
    ** @param  ring1Path      - path to the Ring_1 file, or nullptr if not applicable
    ** @param  ring2Path      - path to the Ring_2 file, or nullptr if not applicable
    ** @param  ring34Path     - path to the Ring_3_4 file (always required)
    ** @return true if every applicable file opened successfully (even if it
    **         turns out to hold zero boards); false only on a missing/
    **         unreadable file.
    */
    bool Open(const char* cellsInUsePath, const char* ring1Path, const char* ring2Path, const char* ring34Path);

    /*
    ** Method: Peek
    ** @brief  Reports the current board without consuming it.
    ** @param  pOutKey - out: the current ring-ordered BOARD_KEY
    ** @return true if a board is available (false at clean EOF or after IsCorrupted()).
    */
    bool Peek(BOARD_KEY* pOutKey) const;

    /*
    ** Method: Advance
    ** @brief  Consumes the current board and reads/computes the next one.
    ** @return true if another board is now available (matches what Peek() would return next).
    */
    bool Advance();

    /*
    ** Method: IsCorrupted
    ** @brief  True if Advance() ever hit EOF on a level where a further
    **         record was expected (a higher level's lookahead already
    **         promised more) -- a truncated/corrupt file, never a
    **         legitimate "no more boards" case. Callers must check this
    **         once Peek() returns false, the same way
    **         RingNestedIndexStreamAll's own false return is never treated
    **         as "no data."
    */
    bool IsCorrupted() const { return corrupted; }

    /*
    ** Method: Close
    ** @brief  Closes every open reader. Safe to call even if Open() failed partway through.
    */
    void Close();

private:
    bool FillCurrent();
};

/*
** Function: RingNestedIndexFileCount
** @brief    Counts how many of the applicable nested-index files exist on
**           disk, without validating their contents. Callers use this to
**           tell "this level/player genuinely has no data" (0 -- Load()
**           would also return false, but for a completely different,
**           expected reason) apart from "data exists but is
**           corrupt/truncated" (1+ but Load() still fails) -- the two must
**           never be handled the same way; the latter is a real problem
**           that should never be silently treated as "no data."
** @param    cellsInUsePath - path to the CellsInUse file
** @param    ring1Path      - path to the Ring_1 file, or nullptr if not applicable for this board size
** @param    ring2Path      - path to the Ring_2 file, or nullptr if not applicable for this board size
** @param    ring34Path     - path to the Ring_3_4 file
** @return   0 if none of the applicable files exist; up to the number of
**           non-null paths passed otherwise. A null path is simply not
**           counted either way (neither present nor absent).
*/
int RingNestedIndexFileCount(const char* cellsInUsePath, const char* ring1Path,
                              const char* ring2Path, const char* ring34Path);
