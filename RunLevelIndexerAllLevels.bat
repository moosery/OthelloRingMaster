@echo off
setlocal enabledelayedexpansion

if "%~1"=="" goto usage
if "%~2"=="" goto usage

set START=%~1
set END=%~2
set EXE=%~dp0x64\Release\OthelloRingMasterLevelIndexer.exe

if not exist "%EXE%" (
    echo ERROR: %EXE% not found -- build Release first.
    exit /b 1
)

echo Running OthelloRingMasterLevelIndexer for levels %START% through %END%.
echo Trying both black and white at every level -- whichever color has no
echo data yet at a given level is just skipped, not treated as an error.
echo.

for /L %%L in (%START%,1,%END%) do (
    for %%C in (black white) do (
        echo === Level %%L, %%C -- started %TIME% ===
        "%EXE%" --level %%L --color %%C
        if errorlevel 1 (
            echo   ^(skipped -- no %%C data at level %%L^)
        )
        echo.
    )
)

echo Done: levels %START% through %END%, finished %TIME%.
goto :eof

:usage
echo Usage: %~nx0 START_LEVEL END_LEVEL
echo.
echo   Runs OthelloRingMasterLevelIndexer.exe for every level in [START_LEVEL,
echo   END_LEVEL], trying both black and white each time.
echo.
echo   Example: %~nx0 0 24
echo.
echo   NOTE: levels 15+ each take real time (Ring_2 and Ring_3_4 both actually
echo   segment at that scale), and levels 20+ can take HOURS EACH now that
echo   Ring_2 segments too, not just Ring_3_4. This script does not prompt
echo   before starting -- pick your range deliberately.
exit /b 1
