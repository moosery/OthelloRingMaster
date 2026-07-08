/*
** Filename:  RetrogradeKernels.h
**
** Purpose:
**   Declares the GPU-side move generation used by the retrograde
**   calculator's non-terminal backward step: given a batch of level N
**   parent boards, generate each parent's children (same move-gen/flip/
**   canonicalize/ring-boundary-conversion logic GpuKernels.cu's
**   ExpandKernel already uses) with the parent<->children association
**   preserved, so the CPU side can look each child up in level N+1's
**   already-computed counts and sum into the correct parent.
**
** Notes:
**   Deliberately NOT the same accumulator shape as GpuKernels.h's
**   GpuAccumulator: that pipeline's whole point is deduping children
**   across a huge shared batch, which is exactly wrong here -- retrograde
**   summing wants every (parent, child) edge counted once, not deduped
**   (see project_adaptive_counter_width_design memory's "multiplicity is
**   deliberate" section). So this kernel is one thread per parent writing
**   into a FIXED, non-atomic, per-thread-private slot range -- no sort,
**   no dedup, no atomics at all. Since ExpandKernel is already one thread
**   per parent internally, this is a smaller variant of it, not a new
**   algorithm.
**
**   A child's color is never stored as a tag -- it's positional, mirroring
**   the same two-stack idea GpuKernels.cu's own accumulator already uses
**   (black grows from one end, white from the other), just scoped per
**   parent thread instead of globally atomic: each parent's fixed slot
**   range packs its black children from the front and white children
**   from the back, growing toward each other. This came out of a direct
**   discussion with the user about not storing a per-record color bit
**   anywhere it can instead be derived from position/file, matching how
**   BOARD_KEY itself carries no next-player bit either. Freeing this one
**   array's worth of memory has a real (if modest) benefit: more headroom
**   to raise this context's own batch size, i.e. more boards processed
**   per GPU round trip -- not, per the user's explicit clarification,
**   about sharing VRAM with a concurrently-running OthelloRingMaster
**   process (the two never run at the same time).
**
**   The color-flip tag is a SEPARATE, unavoidable per-child bit that this
**   restructuring does NOT eliminate: it doesn't say which color a child
**   is (that's now positional), it says whether that child's own
**   ALREADY-COMPUTED on-disk triple was written under a color-swapped
**   canonical form and needs un-swapping before being summed -- a
**   genuinely different question with no positional equivalent found yet.
*/

#pragma once

/* Includes */
#include "RingStoreFile.h"   /* UINT64_PAIR */

/* Structures and Types */

/*
** Type:    RetrogradeGpuContext
** @brief   Opaque GPU context handle -- full definition is internal to
**          RetrogradeKernels.cu.
*/
struct __RetrogradeGpuContext;
typedef struct __RetrogradeGpuContext RetrogradeGpuContext;

/* Functions */

/*
** Function: RetrogradeGpuContextCreate
** @brief    Allocates device + pinned-host buffers sized for batchSize
**           parents at up to maxMovesPerBoard children each.
** @param    boardSize        - board size (4, 6, or 8) -- sets the move-gen masks
** @param    batchSize        - max parents per RetrogradeExpandBatch call
** @param    maxMovesPerBoard - GetMaxMovesForBoardSize(boardSize) -- worst-case children per board
** @return   A new RetrogradeGpuContext.
*/
RetrogradeGpuContext* RetrogradeGpuContextCreate(int boardSize, int batchSize, int maxMovesPerBoard);

/*
** Function: RetrogradeGpuContextDestroy
** @brief    Frees all device/pinned-host memory owned by pCtx.
** @param    pCtx - the context to destroy
*/
void RetrogradeGpuContextDestroy(RetrogradeGpuContext* pCtx);

/*
** Function: RetrogradeExpandBatch
** @brief    H2D-copies count parent boards, generates each one's children
**           (handling the pass/terminal cases exactly like ExpandKernel),
**           and D2H-copies the per-parent results back into host-resident
**           buffers readable via the Get* functions below until the next call.
** @param    pCtx      - the context to run the batch through
** @param    pParents  - ring-ordered parent board records
** @param    count     - number of records in pParents (must be <= the
**                       batchSize passed to RetrogradeGpuContextCreate)
** @param    playerBit - 1=black, 0=white -- next player to move for this whole batch
*/
void RetrogradeExpandBatch(RetrogradeGpuContext* pCtx, const UINT64_PAIR* pParents,
                            int count, uint8_t playerBit);

/*
** Function: RetrogradeGetChildCount
** @brief    Returns parentIdx's child count for one color, from the last
**           RetrogradeExpandBatch call. A parent is terminal iff both
**           colors' counts are 0.
** @param    pCtx      - the context to query
** @param    parentIdx - index within the last batch (0 <= parentIdx < count)
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE -- which color's children to count
** @return   Number of that color's children generated for this parent.
*/
uint32_t RetrogradeGetChildCount(const RetrogradeGpuContext* pCtx, int parentIdx, int player);

/*
** Function: RetrogradeGetChild
** @brief    Returns one child of parentIdx's given color, by logical index
**           in generation order (0 <= childIdx < RetrogradeGetChildCount(parentIdx, player)).
** @param    pCtx             - the context to query
** @param    parentIdx        - index within the last batch
** @param    player           - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE -- which color's children to read
** @param    childIdx         - logical index within that color's children
** @param    pOutBoard        - out: the child's ring-ordered board record
** @param    pOutColorFlipped - out: true if this child's already-computed
**                              on-disk triple was written under a
**                              color-swapped canonical form and needs
**                              blackWins/whiteWins swapped before being
**                              summed (tie count unaffected) -- see file Notes
*/
void RetrogradeGetChild(const RetrogradeGpuContext* pCtx, int parentIdx, int player, int childIdx,
                         UINT64_PAIR* pOutBoard, bool* pOutColorFlipped);
