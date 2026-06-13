# Spatial Canvas

***English** · [Türkçe](README.tr.md)*

![Spatial Canvas demo](assets/demo.gif)

A window manager that places your live Windows windows onto an infinite,
zoomable canvas. Instead of three monitors (or alongside them), one
"bird's-eye workspace": zoom out, see everything; zoom in, teleport into the
real window and work; pull back with a thumb button.

**Version:** 0.48.0 (M0–M50) · **Platform:** Windows 11
(Windows.Graphics.Capture) · **Status:** Active prototype / dogfood

## Install

Requires **Windows 10 1903+ / Windows 11** (for the screen-capture API).
No installation, **single file**: `SpatialCanvas.exe` (statically linked; no
.NET or VC redistributable needed).

1. Download the latest `.zip` (Releases) → extract to a folder.
2. Run `SpatialCanvas.exe`. The canvas opens and captures your open windows.
3. Press **ESC** to exit (everything returns to its place). A 3-line tip card
   shows on first launch; press **F1** for all shortcuts.

> The unsigned build may trigger Windows SmartScreen ("unknown publisher") →
> *More info → Run anyway*. (A signed build is on the roadmap.)
>
> The UI is English by default; switch to Turkish in **Settings → Language**.

## How it works (architecture)

There is NO input simulation. Three layers:

1. **Capture** — A Windows.Graphics.Capture session per window (FreeThreaded
   FramePool, poll-based, lock-free). Frames are copied into persistent D3D11
   textures.
2. **Canvas** — A borderless fullscreen D3D11 swapchain; each window is a
   1:1-pixel quad in world space. A D2D/DWrite overlay draws title labels,
   hover highlight, a world-anchored dot grid, docks, and panels.
3. **Park & Swap** — Windows "park" in a 2px visible strip at the bottom of
   the primary monitor (so occlusion throttling never fires → textures stay
   live). When you cross the zoom threshold, the real HWND teleports onto the
   quad and takes focus; pull back and it returns to the park strip.

In canvas mode the window is TOPMOST and the taskbar is hidden; when you dive
into a window it drops to the normal z-order and the taskbar returns.

## Controls

| Action | Input |
|---|---|
| Zoom (cursor-focused) | Wheel |
| Pan (with momentum) | Middle/right-drag · auto-pan when dragging to an edge |
| Move tile (snaps to edges) | Left-drag · **Alt**+drag = snapping off |
| Move snapped cluster | **Shift**+drag |
| Multi-select | Drag empty space (marquee) · **Delete** = remove selected |
| Dive into a window | Cross zoom threshold · double-click · **Enter** (keyboard focus) |
| Pull back (same view) | **Mouse FORWARD button** · Ctrl+Alt+Wheel-down · Ctrl+Alt+Z |
| Keyboard navigation | **Arrow keys** (direction) · **Tab/Shift+Tab** (most recent) |
| Search windows | **Ctrl+F** · top-center "Search" button |
| Launch app | **Ctrl+N** (palette: type+Enter / 1-9 / click) |
| Duplicate window | **Ctrl+C** / **Ctrl+V** (to cursor position) |
| Pin to screen (HUD) | **Ctrl+P** (ignores pan/zoom; again = unpin) |
| Bookmark | **Ctrl+Shift+1–4** save · **Ctrl+1–4** go |
| Fit all / selection | **F** · Shift+1 / Shift+2 |
| Arrange windows into a grid | **Ctrl+G** (Miro "clean up" style) |
| Keyboard: tile focus / move | **Arrow** (focus) · **Shift+Arrow** (move) |
| Minimap (bird's-eye) | Bottom-right corner · click = jump there (toggle in panel) |
| Sticky note (annotation) | **Ctrl+Shift+N** = note at cursor · type, **Tab**=color, **Enter/Esc**=done |
| Move / edit / resize / delete note | Left-drag · double-click = edit · bottom-right corner = resize · hover **✕** = delete |
| All shortcuts | **F1** |
| **Close** a window (the app) | Hover → **✕** top-right (the app's own save dialog appears) |
| Settings panel | Top-left **⚙** or S |
| Running-window dock | Move cursor to **bottom** edge of primary monitor → click = fly to that window |
| App dock (launcher) | Move cursor to **right** edge → left-click = launch · **+** = add · **right-click** = remove |
| Exit (everything returns) | ESC (first closes any open panel/selection) |

All shortcuts can be reassigned (to a keyboard OR mouse button) from the
**Shortcuts** tab of the Settings panel. A 3-line tip card shows on first launch.

## App launcher

Opens apps without leaving the canvas. Two ways:

- **Right-edge dock** (recommended): move the cursor to the right edge of the
  screen → your saved apps open as a vertical dock with real icons; click an
  icon to launch. The **+** button at the bottom opens a file picker; the
  `.exe` you pick is added to the dock instantly (and saved to `launcher.txt`
  — no manual editing).
- **Ctrl+N palette**: type a free command (`cmd`, `notepad`,
  `chrome --new-window`, a quoted full path) or pick a saved shortcut with
  1-9 / click.

The launched window lands on the canvas at the cursor position. Shortcuts are
stored in `%APPDATA%\SpatialCanvas\launcher.txt` as `Label|program|arguments`
(a sample template is created on first launch).

## Settings panel

Language (English/Türkçe), update check, capture FPS (15/30/60), animation
speed, title labels, hover frame, dive threshold, max window count, background
tone, dot grid, start with Windows, canvas area (primary screen / all monitors).
Persisted in `%APPDATA%\SpatialCanvas\settings.txt`.

> **Update check** (on by default) fetches a small version file from GitHub on
> launch and shows a notification if a newer release exists — notification only,
> nothing is downloaded or installed. Turn it off in Settings if you prefer no
> network calls.

## Resilience

- **Single instance:** a mutex; a second launch brings the existing canvas to front.
- **Crash insurance:** park state is written to `pending_restore.txt`; on top of
  that a crash filter (`SetUnhandledExceptionFilter`) instantly re-shows the
  taskbar and un-parks windows. The next launch also restores stranded windows
  (matched by HWND/exe/title) to their places.
- **Device loss:** TDR / driver update / sleep-wake → D3D/D2D/WGC fully rebuilt;
  if that fails, windows are restored and the app exits cleanly (no window is
  left orphaned in the 2px strip).
- **Resolution/DPI change:** geometry, swapchain, and the park strip are rebuilt live.
- **Session end:** `WM_QUERYENDSESSION`/`WM_ENDSESSION` → windows return to place,
  taskbar comes back.
- **If explorer.exe restarts:** `TaskbarCreated` is observed and the new taskbar
  is hidden again.
- **Lifecycle:** tiles of closed windows are cleaned up; a window whose capture
  died but is still alive isn't left parked — it's returned. Newly opened windows
  auto-tile while in canvas mode.
- **Layout persistence:** tile placement is stored per-exe in `layout.txt`.

## Performance

Doesn't burn the GPU when idle: the main loop draws only when something changes
(dirty flag + `MsgWaitForMultipleObjectsEx`). The dot grid is drawn with a single
wrap-mode bitmap brush; overlay brushes are cached.

## Window rules

`%APPDATA%\SpatialCanvas\rules.txt` (hand-edited): an `exclude=app.exe` line
never captures that exe (a commented template is created on first launch).

## Build

Requirements: VS 2022 (MSVC v143), Windows SDK 10.0.26100.

```
MSBuild Win32CaptureSample.sln -p:Configuration=Release -p:Platform=x64 -m
```

Output: `x64\Release\SpatialCanvas.exe`. The entire app is in a single module:
`Win32CaptureSample/Canvas.cpp` (entry point `RunCanvasApp()`).

## License

Source-available, **proprietary** license — not open source. Anyone may
**download and use** it for free (personal / educational / internal use).
**Copying, reproducing, redistributing, and offering it commercially is
prohibited without written permission.** Details: [LICENSE](LICENSE).

## Acknowledgments

The capture infrastructure builds on the structure of
[robmikh/Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample)
(MIT); the build uses the `robmikh.common` (MIT, NuGet) package. These
third-party components remain under their respective licenses.

---
© 2026 Batuhan Demirbilek · 13auth
