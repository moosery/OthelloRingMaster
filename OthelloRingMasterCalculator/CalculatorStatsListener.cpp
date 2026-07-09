/*
** Filename:  CalculatorStatsListener.cpp
**
** Purpose:
**   Implements the status thread declared in CalculatorStatsListener.h: a
**   tiny TCP server on pConfig->statsPort that responds to STATUS (a
**   human-readable progress report) and STOP (graceful shutdown request)
**   commands from the standalone OthelloRingMasterCalculatorStatus client.
**
** Notes:
**   Mirrors StatsListener.cpp's shape (same FormatDuration/HandleClient/
**   RunStatsListenerJob structure, same accept-loop-with-50ms-poll), but
**   the actual status content is scoped to what this calculator tracks --
**   no drive ledger, no merge-writer/imerge/cascade progress, none of
**   which exist here. See CalculatorLevelStats in CalculatorTypes.h for
**   the full set of fields this reads.
*/

/* Includes */
/* Include winsock2 before any project headers to prevent winsock1 conflicts */
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>

#include "CalculatorStatsListener.h"
#include "RSFFileName.h"
#include <stdio.h>
#include <string.h>

/* Functions */

/*
** Function: FormatDuration
** @brief    Formats a nanosecond duration as "HH:MM:SS".
** @param    nanos   - duration in nanoseconds
** @param    out     - out: formatted string
** @param    outSize - capacity of out
*/
static void FormatDuration(int64_t nanos, char* out, int outSize)
{
    int64_t s = nanos / 1000000000LL;
    snprintf(out, outSize, "%02lld:%02lld:%02lld",
             s / 3600, (s % 3600) / 60, s % 60);
}

/*
** Function: BuildCalculatorStatusResponse
** @brief    Builds the full human-readable STATUS response: current-level
**           live progress (per color and combined), plus the completed-
**           level history table, walked in processing order (deepest
**           level first, since that's the order this calculator works in).
** @param    pCtx    - calculator context
** @param    buf     - out: destination buffer
** @param    bufSize - capacity of buf
*/
static void BuildCalculatorStatusResponse(PCalculatorContext pCtx, char* buf, int bufSize)
{
    POthelloRingMasterCalculatorConfig pCfg = pCtx->pConfig;
    POthelloRingMasterCalculatorState  pSt  = pCtx->pState;

    int  curLevel = (int)pSt->currentLevel;
    char dur[16];

    int n = 0;
    n += snprintf(buf + n, bufSize - n,
                  "OthelloRingMasterCalculator v%s  |  Board: %dx%d  |  Walking level %d down to 0\n",
                  CALCULATOR_VERSION, pCfg->boardSize, pCfg->boardSize, (int)pSt->deepestLevel);
    n += snprintf(buf + n, bufSize - n, "\n");

    /* --- Current level (live stats) --- */
    const CalculatorLevelStats* cur = &pSt->levelStats[curLevel];
    bool    curDone      = (cur->totalNanos > 0);
    int64_t elapsedNanos = curDone ? cur->totalNanos : ClockNanosSinceStart((PClockTick)&cur->startTick);
    FormatDuration(elapsedNanos, dur, sizeof(dur));

    const char* playerStr = (pSt->currentPlayer == RSF_PLAYER_BLACK) ? "black" : "white";
    n += snprintf(buf + n, bufSize - n,
                  "=== Level %d  [%s-to-move]  %s  (%s) ===\n",
                  curLevel, playerStr, curDone ? "done" : "RUNNING", dur);

    uint64_t curProcessed = (pSt->currentPlayer == RSF_PLAYER_BLACK) ? cur->boardsProcessedBlack : cur->boardsProcessedWhite;
    uint64_t curTotal     = (pSt->currentPlayer == RSF_PLAYER_BLACK) ? cur->totalBoardsBlack      : cur->totalBoardsWhite;

    if (curTotal > 0)
    {
        double pct = 100.0 * (double)curProcessed / (double)curTotal;
        n += snprintf(buf + n, bufSize - n,
                      "  %s-to-move progress    : %llu / %llu  (%.3f%%)\n",
                      playerStr, (unsigned long long)curProcessed, (unsigned long long)curTotal, pct);

        if (!curDone && pct > 0.001 && pct < 100.0 && elapsedNanos > 0)
        {
            int64_t etaNanos = (int64_t)((double)elapsedNanos / pct * (100.0 - pct));
            char    etaDur[16];
            FormatDuration(etaNanos, etaDur, sizeof(etaDur));
            n += snprintf(buf + n, bufSize - n, "  Est. time remaining     : %s\n", etaDur);
        }
    }
    else
    {
        n += snprintf(buf + n, bufSize - n, "  %s-to-move progress    : (not yet started)\n", playerStr);
    }

    n += snprintf(buf + n, bufSize - n,
                  "  Black-to-move so far   : %llu boards  (blackWins=%llu whiteWins=%llu ties=%llu)\n",
                  (unsigned long long)cur->boardsProcessedBlack,
                  (unsigned long long)cur->blackToMoveTotals.blackWins,
                  (unsigned long long)cur->blackToMoveTotals.whiteWins,
                  (unsigned long long)cur->blackToMoveTotals.ties);
    n += snprintf(buf + n, bufSize - n,
                  "  White-to-move so far   : %llu boards  (blackWins=%llu whiteWins=%llu ties=%llu)\n",
                  (unsigned long long)cur->boardsProcessedWhite,
                  (unsigned long long)cur->whiteToMoveTotals.blackWins,
                  (unsigned long long)cur->whiteToMoveTotals.whiteWins,
                  (unsigned long long)cur->whiteToMoveTotals.ties);
    n += snprintf(buf + n, bufSize - n,
                  "  Combined so far        : blackWins=%llu whiteWins=%llu ties=%llu\n",
                  (unsigned long long)cur->combinedTotals.blackWins,
                  (unsigned long long)cur->combinedTotals.whiteWins,
                  (unsigned long long)cur->combinedTotals.ties);
    n += snprintf(buf + n, bufSize - n, "\n");

    /* --- Level history table -- processing order is deepest-first, the
    ** opposite of OthelloRingMaster's own 0-first table, since that's the
    ** actual order this calculator works through levels in.
    */
    n += snprintf(buf + n, bufSize - n,
                  "Lvl    BoardsBlk    BoardsWht       BlackWins       WhiteWins            Ties  Duration\n");
    n += snprintf(buf + n, bufSize - n,
                  "---  -----------  -----------  --------------  --------------  --------------  --------\n");
    for (int lvl = (int)pSt->deepestLevel; lvl >= 0; lvl--)
    {
        if (n >= bufSize - 512) break;   /* safety guard -- buffer nearly full */
        const CalculatorLevelStats* ls = &pSt->levelStats[lvl];
        if (ls->totalNanos == 0) continue;   /* not yet completed */
        char lvlDur[16];
        FormatDuration(ls->totalNanos, lvlDur, sizeof(lvlDur));
        n += snprintf(buf + n, bufSize - n,
                      "%3d  %11llu  %11llu  %14llu  %14llu  %14llu  %8s\n",
                      lvl,
                      (unsigned long long)ls->boardsProcessedBlack,
                      (unsigned long long)ls->boardsProcessedWhite,
                      (unsigned long long)ls->combinedTotals.blackWins,
                      (unsigned long long)ls->combinedTotals.whiteWins,
                      (unsigned long long)ls->combinedTotals.ties,
                      lvlDur);
    }

    n += snprintf(buf + n, bufSize - n, "END\n");
    (void)n;
}

/*
** Function: HandleClient
** @brief    Reads one command (STATUS or STOP) from an accepted client
**           socket, responds, and closes the connection.
** @param    client - the accepted client socket
** @param    pCtx   - calculator context
*/
static void HandleClient(SOCKET client, PCalculatorContext pCtx)
{
    char cmd[64] = {};
    int  got     = recv(client, cmd, (int)sizeof(cmd) - 1, 0);
    if (got <= 0) { closesocket(client); return; }

    /* Trim trailing whitespace/newlines */
    for (int i = got - 1; i >= 0 && (cmd[i] == '\r' || cmd[i] == '\n' || cmd[i] == ' '); i--)
        cmd[i] = '\0';

    if (_stricmp(cmd, "STOP") == 0)
    {
        const char* msg = "Stopping...\n";
        send(client, msg, (int)strlen(msg), 0);
        LoggerLog("STOP command received via stats port -- requesting graceful shutdown before the next level starts...\n");
        pCtx->pState->terminateThreads = true;
    }
    else
    {
        char buf[CALC_MAX_LEVELS * 128 + 4096];
        BuildCalculatorStatusResponse(pCtx, buf, sizeof(buf));
        int toSend = (int)strlen(buf);
        int sent   = 0;
        while (sent < toSend)
        {
            int r = send(client, buf + sent, toSend - sent, 0);
            if (r == SOCKET_ERROR) break;
            sent += r;
        }
    }

    closesocket(client);
}

/*
** Function: RunCalculatorStatsListenerJob
** @brief    The status thread's main loop: binds pCtx->pConfig->statsPort
**           and accepts/handles client connections until told to terminate.
** @param    pCtx - calculator context
*/
static void RunCalculatorStatsListenerJob(uint32_t /*thdIdx*/, PCalculatorContext pCtx)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { WSACleanup(); return; }

    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(pCtx->pConfig->statsPort);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listenSock, 4) != 0)
    {
        LoggerLog("Calculator stats listener: failed to bind/listen on port %d\n",
                  (int)pCtx->pConfig->statsPort);
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    LoggerLog("Calculator stats listener running on port %d\n", (int)pCtx->pConfig->statsPort);

    while (!pCtx->pState->terminateStatsListener)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval tv = { 0, 50000 };   /* 50 ms */

        if (select(0, &readSet, nullptr, nullptr, &tv) <= 0)
            continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client != INVALID_SOCKET)
            HandleClient(client, pCtx);
    }

    closesocket(listenSock);
    WSACleanup();
    LoggerLog("Calculator stats listener stopped.\n");
}

/*
** Function: SubmitCalculatorStatsListenerJob
** @brief    See CalculatorStatsListener.h.
*/
void SubmitCalculatorStatsListenerJob(PCalculatorContext pCtx)
{
    pCtx->pState->pStatsThreadPool->QueueJob(
        [pCtx](uint32_t thdIdx)
        {
            RunCalculatorStatsListenerJob(thdIdx, pCtx);
        }
    );
}
