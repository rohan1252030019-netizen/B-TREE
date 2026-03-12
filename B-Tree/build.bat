@echo off
setlocal EnableDelayedExpansion
REM ============================================================
REM build.bat  --  FileExplorer B+ Tree (MinGW / g++ build)
REM
REM Usage:  .\build.bat            (release build)
REM         .\build.bat debug      (debug build with symbols)
REM
REM Prerequisites:
REM   1. MinGW-w64 g++ on PATH (msys64\ucrt64\bin or mingw64\bin)
REM   2. SQLite amalgamation in db\  (run: python fetch_sqlite.py)
REM ============================================================

echo.
echo === FileExplorer B+ Tree Build ===
echo.

REM --- Check for g++ ---
where g++ >nul 2>&1
if errorlevel 1 (
    echo ERROR: g++ not found on PATH.
    echo Install MinGW-w64 and add its bin\ directory to PATH.
    echo  e.g.  C:\msys64\ucrt64\bin   or   C:\mingw64\bin
    pause
    exit /b 1
)

REM --- Check for SQLite amalgamation ---
if not exist "db\sqlite3.c" (
    echo ERROR: db\sqlite3.c not found.
    echo Run:  python fetch_sqlite.py
    pause
    exit /b 1
)
if not exist "db\sqlite3.h" (
    echo ERROR: db\sqlite3.h not found.
    echo Run:  python fetch_sqlite.py
    pause
    exit /b 1
)

REM --- Choose build type ---
set OPTFLAGS=-O2
set DEBUGFLAGS=
if /I "%1"=="debug" (
    set OPTFLAGS=-O0 -g
    set DEBUGFLAGS=-DDEBUG
    echo Build type: DEBUG
) else (
    echo Build type: RELEASE
)

REM --- Compile sqlite3.c separately as C (not C++).
REM
REM  The UCRT-flavored MinGW headers include pthread_time.h when
REM  SQLITE_THREADSAFE != 0.  We disable SQLite's own mutex layer
REM  with SQLITE_THREADSAFE=0 because our own SRWLOCK guards the DB.
REM  This is safe: we call sqlite3_*  from a single thread at a time
REM  (serialized by the FileIndex critical section).
REM
echo Compiling sqlite3.c ...
gcc -O2 -c db/sqlite3.c ^
    -DSQLITE_THREADSAFE=0 ^
    -DSQLITE_DEFAULT_MEMSTATUS=0 ^
    -DSQLITE_OMIT_LOAD_EXTENSION ^
    -o sqlite3.o
if errorlevel 1 (
    echo ERROR: sqlite3.c compile failed.
    echo.
    echo Tip: make sure you are using the ucrt64 or mingw64 gcc, not a
    echo      32-bit or Cygwin compiler.
    pause
    exit /b 1
)
echo   sqlite3.o OK

REM --- Compile the resource file (optional; skip gracefully if windres missing) ---
set RC_OBJ=
where windres >nul 2>&1
if not errorlevel 1 (
    echo Compiling resource.rc ...
    windres resource.rc -O coff -o resource.res 2>nul
    if not errorlevel 1 (
        set RC_OBJ=resource.res
        echo   resource.res OK
    ) else (
        echo   WARNING: windres failed; building without manifest.
    )
) else (
    echo   INFO: windres not found; building without manifest.
)

REM --- Main C++ compile + link ---
echo Compiling and linking ...
g++ -std=c++17 %OPTFLAGS% -mwindows -municode %DEBUGFLAGS% ^
    main1.cpp ^
    bplustree/bplustree.cpp ^
    filesystem/file_manager.cpp ^
    index/file_index.cpp ^
    index/search_engine.cpp ^
    db/db_store.cpp ^
    sqlite3.o ^
    gui/window.cpp ^
    gui/visualizer.cpp ^
    %RC_OBJ% ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 ^
    -Wno-cast-function-type ^
    -o FileExplorerBPlusTree.exe ^
    2>&1

if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    if exist sqlite3.o del sqlite3.o
    if exist resource.res del resource.res
    pause
    exit /b 1
)

REM --- Clean up intermediate files ---
if exist sqlite3.o   del sqlite3.o
if exist resource.res del resource.res

echo.
echo ============================================
echo  BUILD SUCCESSFUL
echo  Output: FileExplorerBPlusTree.exe
echo ============================================
echo.
echo Run: FileExplorerBPlusTree.exe
echo.
pause
