/*
** Filename:  OthelloRingMasterStatus.cpp
**
** Purpose:
**   A tiny standalone TCP client: connects to a running OthelloRingMaster.exe
**   on localhost and prints live level progress, drive stats, and
**   completed-level history (or sends a STOP command for graceful shutdown).
**
** Notes:
**   Adapted from an earlier solver implementation's own status client.
**   Default port changed to 17532 so this and any other status listener
**   on the same dev machine can run concurrently without a bind conflict.
**   Otherwise unchanged -- this file has zero dependency on the rest of
**   the solution.
*/

/* Includes */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#pragma comment(lib, "ws2_32.lib")

/* Constants */
static const int DEFAULT_PORT = 17532;

/* Functions */

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
** @param    prog - argv[0], the program's own invocation name
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --port N    Connect to solver on port N  [default: %d]\n", DEFAULT_PORT);
    printf("  --stop      Send stop command instead of status query\n");
    printf("  --help      Show this help\n\n");
    printf("Connects to a running OthelloRingMaster.exe on localhost and prints\n");
    printf("live level progress, drive stats, and completed-level history.\n");
    printf("The query timestamp is printed by this client (not the solver).\n");
}

/*
** Function: main
** @brief    Parses --port/--stop/--help, connects to the solver's status
**           port, sends STATUS or STOP, and prints whatever the solver sends back.
** @param    argc - argument count
** @param    argv - argument values
** @return   0 on success, 1 on any connection/argument failure.
*/
int main(int argc, char* argv[])
{
    bool doStop = false;
    int  port   = DEFAULT_PORT;

    for (int i = 1; i < argc; i++)
    {
        if (_stricmp(argv[i], "--help") == 0 || _stricmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (_stricmp(argv[i], "--stop") == 0)
            doStop = true;
        else if (_stricmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else
        {
            fprintf(stderr, "ERROR: unknown argument '%s'\n\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        fprintf(stderr, "socket() failed\n");
        WSACleanup();
        return 1;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        fprintf(stderr, "Cannot connect to OthelloRingMaster on port %d\n", port);
        closesocket(s);
        WSACleanup();
        return 1;
    }

    const char* cmd = doStop ? "STOP\n" : "STATUS\n";
    send(s, cmd, (int)strlen(cmd), 0);

    /* Timestamp the query on the client side -- the solver's response
    ** doesn't carry one, and this makes it clear how fresh the printed
    ** status actually is.
    */
    {
        time_t now = time(NULL);
        struct tm t = {};
        localtime_s(&t, &now);
        printf("Queried: %04d-%02d-%02d %02d:%02d:%02d\n",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
               t.tm_hour, t.tm_min, t.tm_sec);
    }

    char buf[4096];
    int got;
    while ((got = recv(s, buf, (int)sizeof(buf) - 1, 0)) > 0)
    {
        buf[got] = '\0';
        printf("%s", buf);
    }

    closesocket(s);
    WSACleanup();
    return 0;
}
