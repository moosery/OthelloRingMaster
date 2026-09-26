#!/usr/bin/env python3
"""
DiscardedResultScan.py

Purpose:
    Finds statements that call a Win32 / C runtime / CUDA function whose result
    must be checked and throw the result away. The compiler cannot find these
    itself: the Windows and C runtime headers do not mark DeleteFileA,
    WriteFile, CloseHandle, fclose, fwrite and friends as must-inspect, so
    neither /analyze (C6031) nor [[nodiscard]] sees them. Functions the solution
    owns ARE marked [[nodiscard]] and enforced by Directory.Build.props
    (warning C4834 is an error), so this scan only covers the outside APIs.

Usage:
    python Tools/DiscardedResultScan.py [repo_root]

    Prints one line per finding: path:line: statement. Exit code 0 if clean,
    1 if anything was found.

How to silence a finding:
    A discard that is genuinely fine (closing a handle that was only read from,
    fflush on stdout, ...) is written (void)Call(...) with a comment saying why.
    The scan skips any statement that starts with (void), an assignment, a
    return, or is inside an if/while/for condition.

Limits:
    A text scan. It sees one-statement-per-line calls that begin a line; it
    cannot see a result stored in a variable that is then never examined, nor a
    helper that swallows a status its own callee returned.
"""

import os
import re
import sys

# Outside APIs whose result carries a success/failure or a count that matters.
CHECKED_APIS = [
    # files
    'DeleteFileA', 'DeleteFileW', 'MoveFileExA', 'MoveFileA', 'CopyFileA', 'CreateFileA', 'CreateFileW',
    'WriteFile', 'ReadFile', 'FlushFileBuffers', 'SetFilePointerEx', 'SetEndOfFile', 'SetFileAttributesA',
    'CreateDirectoryA', 'RemoveDirectoryA', 'GetFileAttributesExA', 'GetDiskFreeSpaceExA',
    # handles / sync
    'CloseHandle', 'CreateEventA', 'SetEvent', 'ResetEvent', 'WaitForSingleObject', 'WaitForMultipleObjects',
    'CreateMutexA', 'ReleaseMutex', 'SetConsoleCtrlHandler', 'GlobalMemoryStatusEx', 'DeviceIoControl',
    # C runtime
    'fopen', 'fclose', 'fflush', 'fwrite', 'fread', 'fseek', '_fseeki64', 'ftell', '_ftelli64', 'setvbuf',
    'remove', 'rename', '_mkdir', 'malloc', 'realloc', 'calloc',
    # sockets
    'WSAStartup', 'socket', 'bind', 'listen', 'accept', 'connect', 'send', 'recv', 'setsockopt', 'select',
    # CUDA
    'cudaMalloc', 'cudaMallocHost', 'cudaMemcpy', 'cudaMemcpyAsync', 'cudaMemset', 'cudaMemsetAsync',
    'cudaMemcpyToSymbol', 'cudaStreamSynchronize', 'cudaDeviceSynchronize', 'cudaGetLastError',
]

SKIP_DIRS = ('.git', 'x64', 'Old', 'lz4', 'OthelloR.', '.vs')
LONE_CALL = re.compile(r'^\s*(' + '|'.join(re.escape(n) for n in CHECKED_APIS) + r')\s*\(')


def is_lone_statement(stripped):
    """True for `Call(...);` (optionally followed by a comment); false for a call that
    is one operand of a condition (a line inside a multi-line if/while)."""
    code = re.sub(r'/\*.*?\*/', '', stripped)      # drop /* */ comments
    code = re.sub(r'//.*$', '', code).strip()          # drop // comments
    if not code.endswith(';'):
        return False
    return not re.search(r'!=|==|\|\||&&|<=|>=', code)


def scan(root):
    findings = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not any(d.startswith(s) or d == s for s in SKIP_DIRS)]
        for name in filenames:
            if not name.endswith(('.cpp', '.h', '.cu', '.cuh', '.c')):
                continue
            path = os.path.join(dirpath, name)
            in_block_comment = False
            with open(path, encoding='utf-8', errors='replace') as fh:
                for lineno, line in enumerate(fh, 1):
                    stripped = line.strip()
                    if in_block_comment:
                        if '*/' in stripped:
                            in_block_comment = False
                        continue
                    if stripped.startswith('/*'):
                        if '*/' not in stripped:
                            in_block_comment = True
                        continue
                    if stripped.startswith(('//', '*')):
                        continue
                    if LONE_CALL.match(line) and is_lone_statement(stripped):
                        findings.append((os.path.relpath(path, root), lineno, stripped[:120]))
    return findings


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    findings = scan(os.path.abspath(root))
    for rel, lineno, text in findings:
        print('%s:%d: %s' % (rel, lineno, text))
    print('%d finding(s)' % len(findings))
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
