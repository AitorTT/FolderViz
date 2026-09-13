# FolderViz

A small WinDirStat-style folder size visualizer for Windows, written in C++20 with the
Win32 API and GDI. No third-party dependencies.

## Features

- **Parallel recursive scan** on a background thread using `FindFirstFileEx` with large
  fetch, keeping the UI responsive. Cancellable.
- **Directory tree** (left) with aggregate sizes, lazily populated for large trees.
- **Virtual file list** (right) with sortable Name / Size / % / Type columns.
- **Squarified treemap** (bottom) of the selected directory, colored by file extension,
  with a legend and hover/selection highlighting.
- **Drill-down**: double-click a directory rectangle or file-list row to navigate.
- Skips junctions/symlinks/reparse points to avoid cycles and double counting; reports
  unreadable directories instead of failing.
- Per-monitor DPI aware, visual styles (Common Controls v6).

## Build

Requirements: Visual Studio 2022 Build Tools with the C++ workload (MSVC + Windows SDK)
and Ninja. The bundled CMake from the VS installation is used automatically.

```powershell
.\build.ps1            # Release
.\build.ps1 -Config Debug
```

This configures `build/` with CMake + Ninja and produces `build\FolderViz.exe`.
To use a system CMake instead, run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage

- Launch `FolderViz.exe` and choose **File > Scan Folder...**, or
- `FolderViz.exe "C:\path\to\folder"` to scan immediately.

Controls:

| Action | Effect |
| --- | --- |
| Click tree node | Show that directory in the list and treemap |
| Click a treemap rect | Highlight the item (status bar shows name/size) |
| Double-click a treemap rect / list row | Drill into a directory |
| Click a list column header | Sort by that column |
| F5 | Rescan the current root |
| Ctrl+O | Choose another folder |

## Project layout

```
src/
  main.cpp        WinMain, common controls + COM init, command line
  app.cpp/.h      Win32 UI: tree, virtual list, treemap window, menus, layout
  scanner.cpp/.h  Parallel recursive scanner + progress/cancel
  fsnode.h        Node tree (files/directories) and aggregation
  treemap.cpp/.h  Squarified treemap layout
  filetype.cpp/.h Extension -> color mapping
  format.h        Human-readable size formatting
app.manifest      Common Controls v6 + PerMonitorV2 DPI awareness
```

## How it works

The scanner pushes the root directory onto a work queue processed by a pool of worker
threads. Each worker enumerates one directory, appends file/directory nodes to that
directory's own `children` vector (safe because a directory is owned by exactly one
worker), and enqueues subdirectories, incrementing an outstanding-task counter. When the
counter reaches zero, the coordinating thread computes bottom-up aggregate sizes and
counts, then notifies the UI. Progress is reported by posting messages throttled to
~60 ms so the UI thread only reads atomic counters.

The treemap uses the squarified algorithm (Bruls, Huizing, van Wijk) to keep rectangle
aspect ratios close to 1, sorting children by descending size.

## Limitations / notes

- Hard links are currently counted once per directory entry rather than deduplicated by
  file identity (Windows hard links are uncommon outside `WinSxS`). Reparse points are
  skipped entirely.
- The treemap shows one directory level at a time (drill down by double-clicking) rather
  than the full recursive map WinDirStat uses.
- No "free space" pseudo-file block is drawn.

## Verification

The scanner/aggregation was cross-checked against PowerShell's `Get-ChildItem -Recurse`:

| Folder | Result |
| --- | --- |
| `src` | 52,487 bytes / 11 files / 0 dirs — exact match |
| project root | 2,353,997 bytes / 42 files / 11 dirs — exact match |
| `C:\Windows\System32` | 11,800,766,460 bytes / 24,256 files / 1,657 dirs — exact match (scan ~103 ms vs ~1022 ms) |

A crafted junction test confirmed reparse points are skipped (1 file / 1 dir / 1 skipped,
size not double counted).
