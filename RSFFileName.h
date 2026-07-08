/*
** Filename:  RSFFileName.h
**
** Purpose:
**   Declares the file-naming convention for every ring-store record file
**   OthelloRingMaster produces (store, writer, imerge, cascade-temp,
**   sentinel), across all three RSF tiers (.rsf/.rsfz/.rsfzl -- see
**   Utility/RingStoreFile.h). Every file is player-specific (black or
**   white turn):
**
**     Store files  (storeDir):          Level_0017_6x6_black_0000.rsf
**                                        Level_0017_6x6_white_0000.rsf
**     Writer files (mwDirectory[i]):    writer_black_0042.rsf
**                                        writer_white_0042.rsf
**     Imerge files (mergeDirectory[i]): imerge_L017_black_0042.rsf
**                                        imerge_L017_white_0042.rsf
**     Cascade temp (tempDir):           cascade_temp_L017_black_0042.rsf
**                                        cascade_temp_L017_white_0042.rsf
**
** Notes:
**   Adapted from an earlier solver implementation's own file-naming
**   module, renamed throughout onto this solution's own naming (prefix
**   RSF, extensions .rsf/.rsfz/.rsfzl). Stays in OthelloRingMaster itself
**   (not Utility) since it's level/player/drive-directory-aware, not
**   generic. The counts-file naming helper is intentionally not ported --
**   the win/tie/loss stats format itself is a separate, not-yet-started
**   future phase.
*/

#pragma once

/* Includes */
#include <stdio.h>
#include <string.h>

/* Constants */
#define RSF_PLAYER_BLACK 1
#define RSF_PLAYER_WHITE 0

/* Magic value embedded at the start of a _complete sentinel that contains
** serialized LevelStats. Zero-byte sentinel files (legacy / manually
** created) have no magic and are treated as "level complete, stats unknown."
*/
#define RSF_SENTINEL_STATS_MAGIC 0x5253465354415453ULL   /* "RSFSTATS" in ASCII byte order */

/* Functions */

/*
** Function: RSFPlayerStr
** @brief    Returns the filename-token string for a player.
** @param    player - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @return   "black" or "white".
*/
static inline const char* RSFPlayerStr(int player)
{
    return player ? "black" : "white";
}

/*
** Function: RSFPlayerFromPath
** @brief    Determines a file's player by scanning its filename (not any
**           directory component) for the literal strings "black"/"white".
** @param    path - the file path to inspect
** @return   RSF_PLAYER_BLACK, RSF_PLAYER_WHITE, or -1 if neither is found.
*/
static inline int RSFPlayerFromPath(const char* path)
{
    const char* p = path + strlen(path);
    while (p > path && *p != '\\' && *p != '/') --p;
    if (strstr(p, "black")) return RSF_PLAYER_BLACK;
    if (strstr(p, "white")) return RSF_PLAYER_WHITE;
    return -1;
}

/*
** ============================================================
** Store files
** ============================================================
*/

/*
** Function: RSFNameStoreFile
** @brief    Builds a plain (.rsf) store file path.
** @param    out       - buffer to receive the path
** @param    outSize   - size of out
** @param    dir       - store directory
** @param    boardSize - board size (e.g. 6 for 6x6)
** @param    level     - level number
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    fileIdx   - file index within this level/player
*/
static inline void RSFNameStoreFile(char* out, size_t outSize,
                                     const char* dir, int boardSize,
                                     int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.rsf",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFPatternStoreFiles
** @brief    Builds a wildcard pattern matching all plain store files for one level/player.
*/
static inline void RSFPatternStoreFiles(char* out, size_t outSize,
                                         const char* dir, int boardSize,
                                         int level, int player)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_*.rsf",
             dir, level, boardSize, boardSize, RSFPlayerStr(player));
}

/*
** Function: RSFPatternAnyStoreFiles
** @brief    Builds a wildcard pattern matching all plain store files for one level (both players).
*/
static inline void RSFPatternAnyStoreFiles(char* out, size_t outSize,
                                            const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_*.rsf",
             dir, level, boardSize, boardSize);
}

/*
** ============================================================
** Ring nested-index store files (the actual final per-level store format --
** see OthelloBasics/RingNestedIndex.h). Up to four files per level/player
** (Ring_1/Ring_2 only exist for board sizes that use them -- see
** RingNestedIndexHasRing1/HasRing2), always fileIdx 0 -- sharding into
** multiple files per level/player was never actually used by any caller of
** the plain .rsf naming above either.
** ============================================================
*/

/*
** Function: RSFNameCellsInUseFile
** @brief    Builds the CellsInUse nested-index file path for one level/player.
*/
static inline void RSFNameCellsInUseFile(char* out, size_t outSize,
                                          const char* dir, int boardSize,
                                          int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.cellsinuse",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFNameRing1File
** @brief    Builds the Ring_1 nested-index file path for one level/player.
*/
static inline void RSFNameRing1File(char* out, size_t outSize,
                                     const char* dir, int boardSize,
                                     int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.ring1",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFNameRing2File
** @brief    Builds the Ring_2 nested-index file path for one level/player.
*/
static inline void RSFNameRing2File(char* out, size_t outSize,
                                     const char* dir, int boardSize,
                                     int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.ring2",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFNameRing34File
** @brief    Builds the Ring_3_4 nested-index file path for one level/player.
*/
static inline void RSFNameRing34File(char* out, size_t outSize,
                                      const char* dir, int boardSize,
                                      int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.ring34",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** ============================================================
** Writer files (NVMe MW buffers)
** ============================================================
*/

/*
** Function: RSFNameWriterFile
** @brief    Builds a plain (.rsf) merge-writer output file path.
*/
static inline void RSFNameWriterFile(char* out, size_t outSize,
                                      const char* dir, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\writer_%s_%04d.rsf",
             dir, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFPatternWriterFiles
** @brief    Builds a wildcard pattern matching all plain writer files for one player.
*/
static inline void RSFPatternWriterFiles(char* out, size_t outSize,
                                          const char* dir, int player)
{
    snprintf(out, outSize, "%s\\writer_%s_*.rsf", dir, RSFPlayerStr(player));
}

/*
** ============================================================
** Intermediate merge files (medium drives)
** ============================================================
*/

/*
** Function: RSFNameImergeFile
** @brief    Builds a plain (.rsf) intermediate-merge output file path.
*/
static inline void RSFNameImergeFile(char* out, size_t outSize,
                                      const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_%04d.rsf",
             dir, level, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFPatternImergeFiles
** @brief    Builds a wildcard pattern matching all plain imerge files for one level/player.
*/
static inline void RSFPatternImergeFiles(char* out, size_t outSize,
                                          const char* dir, int level, int player)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_*.rsf",
             dir, level, RSFPlayerStr(player));
}

/*
** ============================================================
** Compressed store files (.rsfz)
** Same naming as store files but with .rsfz extension. Used when
** --compress is active; RSFOpen detects format from the trailer magic.
** ============================================================
*/

/*
** Function: RSFZNameStoreFile
** @brief    Builds a delta+varint-compressed (.rsfz) store file path.
*/
static inline void RSFZNameStoreFile(char* out, size_t outSize,
                                      const char* dir, int boardSize,
                                      int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.rsfz",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFZPatternStoreFiles
** @brief    Builds a wildcard pattern matching all .rsfz store files for one level/player.
*/
static inline void RSFZPatternStoreFiles(char* out, size_t outSize,
                                          const char* dir, int boardSize,
                                          int level, int player)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_*.rsfz",
             dir, level, boardSize, boardSize, RSFPlayerStr(player));
}

/*
** Function: RSFZPatternAnyStoreFiles
** @brief    Builds a wildcard pattern matching all .rsfz store files for one level (both players).
*/
static inline void RSFZPatternAnyStoreFiles(char* out, size_t outSize,
                                             const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_*.rsfz",
             dir, level, boardSize, boardSize);
}

/*
** ============================================================
** Cascade temp files
** ============================================================
*/

/*
** Function: RSFNameCascadeTemp
** @brief    Builds a plain (.rsf) cascade-merge temp file path.
*/
static inline void RSFNameCascadeTemp(char* out, size_t outSize,
                                       const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\cascade_temp_L%03d_%s_%04d.rsf",
             dir, level, RSFPlayerStr(player), fileIdx);
}

/*
** ============================================================
** Compressed writer files (.rsfz)
** ============================================================
*/

/*
** Function: RSFZNameWriterFile
** @brief    Builds a delta+varint-compressed (.rsfz) merge-writer output file path.
*/
static inline void RSFZNameWriterFile(char* out, size_t outSize,
                                       const char* dir, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\writer_%s_%04d.rsfz",
             dir, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFZPatternWriterFiles
** @brief    Builds a wildcard pattern matching all .rsfz writer files for one player.
*/
static inline void RSFZPatternWriterFiles(char* out, size_t outSize,
                                           const char* dir, int player)
{
    snprintf(out, outSize, "%s\\writer_%s_*.rsfz", dir, RSFPlayerStr(player));
}

/*
** ============================================================
** Compressed intermediate merge files (.rsfz)
** ============================================================
*/

/*
** Function: RSFZNameImergeFile
** @brief    Builds a delta+varint-compressed (.rsfz) intermediate-merge output file path.
*/
static inline void RSFZNameImergeFile(char* out, size_t outSize,
                                       const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_%04d.rsfz",
             dir, level, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFZPatternImergeFiles
** @brief    Builds a wildcard pattern matching all .rsfz imerge files for one level/player.
*/
static inline void RSFZPatternImergeFiles(char* out, size_t outSize,
                                           const char* dir, int level, int player)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_*.rsfz",
             dir, level, RSFPlayerStr(player));
}

/*
** ============================================================
** Compressed cascade temp files (.rsfz)
** ============================================================
*/

/*
** Function: RSFZNameCascadeTemp
** @brief    Builds a delta+varint-compressed (.rsfz) cascade-merge temp file path.
*/
static inline void RSFZNameCascadeTemp(char* out, size_t outSize,
                                        const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\cascade_temp_L%03d_%s_%04d.rsfz",
             dir, level, RSFPlayerStr(player), fileIdx);
}

/*
** ============================================================
** LZ4-compressed store files (.rsfzl)
** Same naming as .rsfz store files but with .rsfzl extension. Used when
** storeDrive is listed in lz4Drives config.
** ============================================================
*/

/*
** Function: RSFZLNameStoreFile
** @brief    Builds a delta+varint+LZ4-compressed (.rsfzl) store file path.
*/
static inline void RSFZLNameStoreFile(char* out, size_t outSize,
                                       const char* dir, int boardSize,
                                       int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_%04d.rsfzl",
             dir, level, boardSize, boardSize, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFZLPatternStoreFiles
** @brief    Builds a wildcard pattern matching all .rsfzl store files for one level/player.
*/
static inline void RSFZLPatternStoreFiles(char* out, size_t outSize,
                                           const char* dir, int boardSize,
                                           int level, int player)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s_*.rsfzl",
             dir, level, boardSize, boardSize, RSFPlayerStr(player));
}

/*
** Function: RSFZLPatternAnyStoreFiles
** @brief    Builds a wildcard pattern matching all .rsfzl store files for one level (both players).
*/
static inline void RSFZLPatternAnyStoreFiles(char* out, size_t outSize,
                                              const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_*.rsfzl",
             dir, level, boardSize, boardSize);
}

/*
** ============================================================
** LZ4-compressed writer files (.rsfzl)
** varint + LZ4 frame; used on drives listed in lz4Drives config.
** Store files stay .rsfz (ZFS on the store drive handles additional compression).
** ============================================================
*/

/*
** Function: RSFZLNameWriterFile
** @brief    Builds a delta+varint+LZ4-compressed (.rsfzl) merge-writer output file path.
*/
static inline void RSFZLNameWriterFile(char* out, size_t outSize,
                                        const char* dir, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\writer_%s_%04d.rsfzl",
             dir, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFZLPatternWriterFiles
** @brief    Builds a wildcard pattern matching all .rsfzl writer files for one player.
*/
static inline void RSFZLPatternWriterFiles(char* out, size_t outSize,
                                            const char* dir, int player)
{
    snprintf(out, outSize, "%s\\writer_%s_*.rsfzl", dir, RSFPlayerStr(player));
}

/*
** ============================================================
** LZ4-compressed intermediate merge files (.rsfzl)
** ============================================================
*/

/*
** Function: RSFZLNameImergeFile
** @brief    Builds a delta+varint+LZ4-compressed (.rsfzl) intermediate-merge output file path.
*/
static inline void RSFZLNameImergeFile(char* out, size_t outSize,
                                        const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_%04d.rsfzl",
             dir, level, RSFPlayerStr(player), fileIdx);
}

/*
** Function: RSFZLPatternImergeFiles
** @brief    Builds a wildcard pattern matching all .rsfzl imerge files for one level/player.
*/
static inline void RSFZLPatternImergeFiles(char* out, size_t outSize,
                                            const char* dir, int level, int player)
{
    snprintf(out, outSize, "%s\\imerge_L%03d_%s_*.rsfzl",
             dir, level, RSFPlayerStr(player));
}

/*
** ============================================================
** LZ4-compressed cascade temp files (.rsfzl)
** ============================================================
*/

/*
** Function: RSFZLNameCascadeTemp
** @brief    Builds a delta+varint+LZ4-compressed (.rsfzl) cascade-merge temp file path.
*/
static inline void RSFZLNameCascadeTemp(char* out, size_t outSize,
                                         const char* dir, int level, int player, int fileIdx)
{
    snprintf(out, outSize, "%s\\cascade_temp_L%03d_%s_%04d.rsfzl",
             dir, level, RSFPlayerStr(player), fileIdx);
}

/*
** ============================================================
** Level sentinel files (storeDir)
** Zero-byte (or LevelStats-bearing) markers written to storeDir to track
** merge state:
**   Level_NNNN_WxH_merging  -- created at the START of the end-of-level merge
**   Level_NNNN_WxH_complete -- created when BOTH player threads finish
**                              cleanly; the merging sentinel is deleted at
**                              the same time.
** The resume scan uses these to distinguish a complete level (has
** _complete) from an interrupted one (has _merging but no _complete).
** Board size is embedded (matching the store-file naming convention) so
** sentinels from different board-size runs sharing a storeDir can never be
** mistaken for each other.
** ============================================================
*/

/*
** Function: SentinelNameMerging
** @brief    Builds the "merge in progress" sentinel file path for a level.
*/
static inline void SentinelNameMerging(char* out, size_t outSize,
                                        const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_merging", dir, level, boardSize, boardSize);
}

/*
** Function: SentinelNameComplete
** @brief    Builds the "level complete" sentinel file path for a level.
*/
static inline void SentinelNameComplete(char* out, size_t outSize,
                                         const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_complete", dir, level, boardSize, boardSize);
}
