@echo off
rem NetHack Modern: start NetHackW in wizard (debug) mode for testing.
rem
rem  - runs the build in vsbinary\ (build the solution first)
rem  - turns on the fx event log: vsbinary\fxlog.txt (appended to)
rem  - wizard mode needs WIZARDS=* (or your user name) in the sysconf
rem    file, normally C:\ProgramData\NetHack\5.0\sysconf
rem
rem Useful wizard-mode keys: Ctrl-W wish, Ctrl-G create monster,
rem #wizintrinsic give a status, Ctrl-T teleport, Ctrl-V level teleport,
rem Ctrl-F map level, Ctrl-E find hidden things, Ctrl-X show attributes.

setlocal
set "ROOT=%~dp0.."
set "BIN=%ROOT%\vsbinary"

if not exist "%BIN%\NetHackW.exe" (
    echo NetHackW.exe not found in "%BIN%".
    echo Build sys\windows\vs\NetHack.sln first.
    pause
    exit /b 1
)

set "NETHACK_FXLOG=%BIN%\fxlog.txt"
rem -u wizard: wizard mode is only granted to the player named "wizard";
rem this also keeps its save file separate from your normal games
start "" /D "%BIN%" "%BIN%\NetHackW.exe" -D -u wizard
