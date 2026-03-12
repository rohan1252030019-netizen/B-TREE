# FileExplorer B+ Tree

**Advanced File Management System with Persistent Storage, Real Filesystem Operations & Interactive B+ Tree Visualizer**

Windows Desktop Application — C++ / Win32 API  
Advanced Data Structures Course Project

---

## Quick Start (one command after setup)

```bat
REM 1. Fetch SQLite amalgamation (one-time)
python fetch_sqlite.py

REM 2. Build (MinGW / g++)
build.bat

REM 3. Run
FileExplorerBPlusTree.exe
```

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| MinGW-w64 / g++ | ≥ 9.0 | With C++17 support (`-std=c++17`) |
| Python 3 | any | Only needed to run `fetch_sqlite.py` |
| Windows | 7 / 10 / 11 | 64-bit recommended |

---

## Step-by-Step Build

### Step 1 — Get SQLite amalgamation

```
python fetch_sqlite.py
```

This downloads `sqlite3.h` and `sqlite3.c` into `db/`.  
Alternatively download `sqlite-amalgamation-*.zip` from <https://www.sqlite.org/download.html> and extract manually.

### Step 2 — Build with MinGW

```bat
build.bat
```

The script runs:

```bat
g++ -std=c++17 -O2 -mwindows -municode ^
    main.cpp ^
    bplustree/bplustree.cpp ^
    filesystem/file_manager.cpp ^
    index/file_index.cpp index/search_engine.cpp ^
    db/db_store.cpp db/sqlite3.c ^
    gui/window.cpp gui/visualizer.cpp ^
    resource.rc ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 ^
    -o FileExplorerBPlusTree.exe
```

### Step 3 — Build with Visual Studio 2022

1. **New Project** → Empty C++ Project
2. Add all `.cpp` and `.h` files; include `db/sqlite3.c` as a **C** file (right-click → Properties → C/C++ → Compile As → C)
3. **Project Properties** → Linker → Additional Dependencies: `comctl32.lib;comdlg32.lib;shell32.lib;ole32.lib`
4. **Character Set** → Unicode
5. **F7** to build

---

## Features

### Real Filesystem Operations

Every operation calls Windows APIs and modifies actual files:

| Button / Menu | API Used | Effect |
|---------------|----------|--------|
| New File | `CreateFile` | Creates empty file on disk |
| New Folder | `CreateDirectory` | Creates real directory |
| Rename | `MoveFileEx` | Atomic rename |
| Delete | `SHFileOperation` | Sends to Recycle Bin |
| Move | `MoveFileEx` | Moves across directories |
| Open | `ShellExecute` | Opens in default app |

### B+ Tree Index

Three in-memory B+ Trees (order `t=4`, max 7 keys/node):
- **Name tree** — keyed by lowercase filename
- **Size tree** — keyed by `uint64_t` size in bytes  
- **Date tree** — keyed by `time_t` modification timestamp

All trees use doubly-linked leaves for O(n) sequential scan and O(log n + k) range queries.

### Persistent SQLite Index

`file_index.db` is created in the executable's directory.  
On startup: load all rows → rebuild in-memory trees → background integrity sweep removes stale entries.  
Every mutation writes to both B+ Trees and SQLite atomically.

### Search Engine

| Search Type | Mechanism |
|-------------|-----------|
| Exact filename | B+ Tree exact match, O(log n) |
| Prefix search | Leaf linked-list scan from first match |
| Extension filter | Linear scan of name tree |
| Size range | Range query on size tree |
| Date range | Range query on date tree |

Search auto-triggers after 300 ms typing debounce. Press **Esc** to clear.

### B+ Tree Visualizer

- Live node rendering (internal = dark blue, leaf = teal)
- Leaf-to-leaf arrows showing linked-list structure
- **Animated splits**: promoted key node turns orange for ~400 ms
- **Animated merges**: merged node turns purple
- **Search highlight**: traversed nodes flash blue
- Mouse-wheel zoom, click-drag pan
- Tree selector: **Name Tree / Size Tree / Date Tree**
- Double-buffered GDI rendering at ~30 FPS

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Win32 GUI Layer                                         │
│  AppWindow (WndProc) · TreeView · ListView · Visualizer  │
└───────────────────────┬──────────────────────────────────┘
                        │ events / callbacks
┌───────────────────────▼──────────────────────────────────┐
│  Application Controller (AppWindow methods)              │
│  Orchestrates FS Manager, FileIndex, SearchEngine        │
└────┬──────────────────┬──────────────────┬───────────────┘
     │                  │                  │
┌────▼────┐   ┌─────────▼──────┐  ┌───────▼──────────────┐
│   FS    │   │  FileIndex     │  │  BPlusTree<K,V,t>    │
│ Manager │   │  (3 trees      │  │  Generic template    │
│ Win32   │   │  + SQLite sync)│  │  3 instances         │
└─────────┘   └─────────┬──────┘  └──────────────────────┘
                        │
              ┌─────────▼──────┐
              │  DBStore       │
              │  sqlite3.c     │
              │  file_index.db │
              └────────────────┘
```

### Module Map

| Module | Files | Responsibility |
|--------|-------|----------------|
| **BPlusTree** | `bplustree/bplustree.h` | Generic template B+ Tree; insert, delete, search, range scan, split, merge, leaf linking |
| **FileRecord** | `models/file_record.h` | POD struct: filename, extension, size, path, timestamps |
| **FileManager** | `filesystem/file_manager.*` | Win32 API wrappers; fires callbacks to update index |
| **FileIndex** | `index/file_index.*` | Maintains 3 B+ Trees; syncs with SQLite; background integrity thread |
| **SearchEngine** | `index/search_engine.*` | Wraps B+ Tree query APIs; returns sorted result vectors |
| **AppWindow** | `gui/window.*` | WinMain, WndProc, TreeView, ListView, menus, toolbar, status bar |
| **Visualizer** | `gui/visualizer.*` | Custom GDI B+ Tree painter; animation loop via WM_TIMER |
| **DBStore** | `db/db_store.*` | SQLite RAII wrapper; CRUD helpers, startup rebuild |
| **Main** | `main.cpp` | wWinMain entry; creates AppWindow |

---

## Non-obvious Architectural Decisions

### Template-only BPlusTree

`BPlusTree<K, V, t>` is entirely in the header because it is a C++ template. Instantiated three times: `BPlusTree<wstring, FileRecord, 4>`, `BPlusTree<uint64_t, FileRecord, 4>` (×2 for size and date).

### Tiebreak on Duplicate Keys

Files with identical names or sizes get a secondary sort key — the full path string — so `(key, tiebreak)` pairs are always unique. This avoids the complexity of multi-value nodes while preserving correct B+ Tree ordering.

### SRWLOCK per Tree

Each tree has its own `SRWLOCK` (Slim Reader-Writer Lock). Multiple threads can read concurrently; writes are exclusive. The background integrity sweep runs under the DB critical section, not the tree locks, so it does not block search queries.

### Double-Buffered GDI Visualizer

The visualizer keeps an off-screen `HBITMAP` the size of the client area. `WM_PAINT` composites to the screen via a single `BitBlt`. `WM_ERASEBKGND` is suppressed (returns 1) to eliminate flicker. `WM_TIMER` at 33 ms (~30 FPS) drives the animation tick counter.

### Snapshot for Visualizer

The visualizer reads a `NodeSnapshot` (type-erased tree structure) rather than holding a pointer into the live tree. This avoids lock contention: the main thread takes a read lock, copies the snapshot, releases the lock, then paints.

### Background Integrity Thread

On startup, after the B+ Trees are loaded from SQLite, a background thread calls `GetFileAttributesW` on every indexed path and prunes stale records. It fires `PostMessage(hwnd_, WM_INDEX_COMPLETE, ...)` when done so the UI thread can refresh safely without locking.

### ListView Populated from B+ Tree

`AppWindow::populateListView()` calls `fileIndex_->getByDirectory(currentDir_)` which does an O(n) scan of the name tree leaf list, filtering by parent directory. Sort columns are implemented by re-traversing the appropriate tree (name: ascending/descending leaf walk; size/date: sorted tree leaf walk) — no `std::sort` is used on pre-sorted data.

---

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| F2 | Rename selected item |
| Delete | Delete selected item |
| Enter | Open selected item |
| Esc | Clear search and restore directory view |
| Ctrl+N | New File |
| Ctrl+Shift+N | New Folder |

---

## File Layout

```
/FileExplorerBPlusTree
├── main.cpp                  ← wWinMain, module wiring
├── resource.rc               ← Win32 resources (manifest, version)
├── build.bat                 ← One-command MinGW build
├── fetch_sqlite.py           ← Downloads SQLite amalgamation
├── README.md
│
├── bplustree/
│   ├── bplustree.h           ← Template BPlusTree<K,V,t> (header-only)
│   └── bplustree.cpp         ← Translation unit stub
│
├── models/
│   └── file_record.h         ← FileRecord POD struct
│
├── filesystem/
│   ├── file_manager.h
│   └── file_manager.cpp      ← Win32 CRUD wrappers
│
├── index/
│   ├── file_index.h
│   ├── file_index.cpp        ← 3 B+ Trees + SQLite sync logic
│   ├── search_engine.h
│   └── search_engine.cpp     ← Search / range query façade
│
├── db/
│   ├── db_store.h
│   ├── db_store.cpp          ← SQLite RAII wrapper
│   ├── sqlite3.h             ← ← Place amalgamation here
│   └── sqlite3.c             ←   (run fetch_sqlite.py)
│
└── gui/
    ├── window.h
    ├── window.cpp            ← WndProc, TreeView, ListView, commands
    ├── visualizer.h
    └── visualizer.cpp        ← GDI B+ Tree painter, animations
```

---

*Document prepared for the Advanced Data Structures course.*  
*All operations target real files on disk. Persistent index via SQLite. Visualizer powered by Win32 GDI.*
