# Spatial Canvas

***English** · [Türkçe](README.tr.md)*

[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6?logo=windows)](#install)
[![Latest release](https://img.shields.io/github/v/release/13auth/spatial-canvas?label=download)](https://github.com/13auth/spatial-canvas/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/13auth/spatial-canvas/total?label=downloads)](https://github.com/13auth/spatial-canvas/releases)
[![License](https://img.shields.io/badge/license-source--available-orange)](LICENSE)

**An infinite, zoomable workspace for your *real, running* apps.** Not
screenshots, not thumbnails — the live windows themselves. Zoom out to see
everything at once, zoom in to teleport into the actual window and work, pull
back with a thumb button. Then annotate the whole thing like a whiteboard.

![Spatial Canvas demo](assets/demo.gif)

> Think *Stage Manager / Mission Control*, but on an **infinite 2D canvas with
> persistent positions** — plus a FigJam-style layer (sticky notes, zone frames,
> connector arrows) drawn directly on top of your live apps.

## Quick start

Requires **Windows 10 1903+ / Windows 11**. Single file, no installer, no
.NET / VC redistributable.

1. **[Download the latest `.zip`](https://github.com/13auth/spatial-canvas/releases/latest)** → extract.
2. Run `SpatialCanvas.exe`. The canvas opens and captures your open windows.
3. Press **ESC** to exit — everything returns exactly where it was. First launch
   shows a 3-line tip card; **F1** lists every shortcut.

> The build is unsigned, so Windows SmartScreen may say "unknown publisher" →
> *More info → Run anyway*. The UI is English by default (switch to Turkish in
> **Settings → Language**).

## Why

More windows than monitors? Instead of alt-tab roulette or a third screen, lay
everything out **spatially** on one canvas and rely on muscle memory for *where*
things are. Built for people who juggle many live windows at once — developers
with a wall of terminals/editors, researchers with a dozen PDFs and docs,
anyone who thinks better in 2D space than in a taskbar.

The key difference from a normal "zoom out" view: the tiles are **live and
interactive after you dive in**. Cross the zoom threshold (or double-click) and
the real window snaps under your cursor with focus — **no input simulation, no
screenshot proxy.** Pull back and it parks again, textures still live.

## Features

- 🪟 **Spaces (multiple canvases)** — virtual-desktop-style spaces (`Ctrl+T` new,
  `Ctrl+Tab` switch). Each keeps its own windows, camera, and notes/zones; the same
  window can live on several spaces, each at its own position (`Ctrl+Alt+1..9`).
  Top switcher strip (`[1][2][3] [+] [×]`); persists across restarts.
- 🔦 **Focus mode** — `Ctrl+Shift+D` dims everything but the window/app under the
  cursor (or your selection), so one context stands out in a wall of windows.
- 🗺️ **Infinite zoomable canvas** — cursor-focused zoom, momentum pan, edge
  auto-pan, world-anchored dot grid. Windows keep fixed world positions.
- 🪂 **Park & Swap dive** — zoom in to enter the real `HWND`; thumb-button (or
  `Ctrl+Alt+Z`) to pull back to the same view.
- 🧭 **Find your way** — `Ctrl+F` search (windows, notes, zones), minimap with
  click-to-jump and drag-to-pan, bookmarks (`Ctrl+1–4`), keyboard navigation.
- 🧩 **Spatial-thinking layer** — sticky **notes** (`Ctrl+Shift+N`), labeled
  **zone frames** that group and tidy the windows inside them (`Ctrl+Shift+Z`),
  and **connector arrows** between windows (`Ctrl+drag`). Undo with `Ctrl+Z`.
- 🚀 **Launch without leaving** — right-edge app dock (real icons) and a
  `Ctrl+N` command palette.
- 📌 **Pin, duplicate, arrange** — pin a window as a screen-fixed HUD
  (`Ctrl+P`), duplicate (`Ctrl+C/V`), arrange into a grid (`Ctrl+G`), export the
  canvas to PNG (`Ctrl+Shift+S`).
- 💾 **Remembers everything** — per-window layout, notes, zones, bookmarks, and
  settings persist across sessions.

<details>
<summary><b>Full controls</b> (or just press F1 in-app)</summary>

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
| Minimap (bird's-eye) | Bottom-right corner · click = jump · drag = pan |
| Sticky note (annotation) | **Ctrl+Shift+N** (or **double-click** empty canvas) · type, **Tab**=color, **Enter/Esc**=done |
| Move / edit / resize / delete note | Left-drag · double-click = edit · bottom-right corner = resize · hover **✕** = delete |
| Zone frame (group windows) | **Ctrl+Shift+Z** = labeled region · drag the **title bar** to move it **and the windows inside** (body is click-through) · corner = resize |
| Connector arrow (+ label) | **Ctrl+drag** window→window · type a label, **Enter** · midpoint **✕** removes · **persists** across restarts |
| Export canvas to PNG | **Ctrl+Shift+S** (saved to your Pictures folder) |
| Undo last delete | **Ctrl+Z** (restores the last removed note / zone / connector) |
| **Space:** new / switch | **Ctrl+T** / **Ctrl+Tab** (Ctrl+Shift+Tab back) |
| **Space:** add/remove window | **Ctrl+Alt+1–9** (pointed or selected window) |
| **Space:** switch / new / delete | Top switcher strip: tab · **+** · **×** |
| **Focus mode** (dim the rest) | **Ctrl+Shift+D** |
| All shortcuts | **F1** |
| **Close** a window (the app) | Hover → **✕** top-right (the app's own save dialog appears) |
| Settings panel | Top-left **⚙** or S |
| Running-window dock | Move cursor to **bottom** edge of primary monitor → click = fly to that window |
| App dock (launcher) | Move cursor to **right** edge → left-click = launch · **+** = add · **right-click** = remove |
| Exit (everything returns) | ESC (first closes any open panel/selection) |

Every shortcut can be reassigned to a keyboard **or** mouse button from the
**Shortcuts** tab in Settings.
</details>

## How it works

There is **no input simulation** and **no screenshot proxy**. Three layers:

1. **Capture** — a Windows.Graphics.Capture session per window (FreeThreaded
   FramePool, poll-based, lock-free). Frames are copied into persistent D3D11
   textures.
2. **Canvas** — a borderless fullscreen D3D11 swapchain; each window is a
   1:1-pixel quad in world space. A D2D/DWrite overlay draws title labels, hover
   highlight, the dot grid, docks, notes, zones, and connectors.
3. **Park & Swap** — windows "park" in a 2px visible strip at the bottom of the
   primary monitor, so occlusion throttling never fires and textures stay live.
   Cross the zoom threshold and the real `HWND` teleports onto the quad and takes
   focus; pull back and it returns to the park strip.

In canvas mode the window is TOPMOST and the taskbar is hidden; diving into a
window drops it to the normal z-order and brings the taskbar back.

<details>
<summary><b>Resilience</b> — what happens on crash, device loss, logoff…</summary>

- **Single instance:** a mutex; a second launch brings the existing canvas to front.
- **Crash insurance:** park state is written to `pending_restore.txt`; a crash
  filter (`SetUnhandledExceptionFilter`) instantly re-shows the taskbar and
  un-parks windows. The next launch also restores stranded windows (matched by
  HWND/exe/title).
- **Device loss:** TDR / driver update / sleep-wake → D3D/D2D/WGC fully rebuilt;
  if that fails, windows are restored and the app exits cleanly (none left
  orphaned in the 2px strip).
- **Resolution/DPI change:** geometry, swapchain, and the park strip rebuild live.
- **Session end:** `WM_QUERYENDSESSION`/`WM_ENDSESSION` → windows return, taskbar
  comes back.
- **explorer.exe restart:** `TaskbarCreated` is observed and the new taskbar is
  hidden again.
- **Lifecycle:** tiles of closed windows are cleaned up; a window whose capture
  died but is still alive is returned, not left parked.

**Performance:** doesn't burn the GPU when idle — the main loop draws only when
something changes (dirty flag + `MsgWaitForMultipleObjectsEx`). The dot grid uses
a single wrap-mode bitmap brush; overlay brushes are cached.
</details>

<details>
<summary><b>Config files</b> — settings, launcher, window rules</summary>

All under `%APPDATA%\SpatialCanvas\`:

- **`settings.txt`** — language, update check, capture FPS (15/30/60), animation
  speed, labels, dive threshold, max window count, background tone, dot grid,
  start-with-Windows, canvas area (primary / all monitors). Edit in-app via the
  **⚙** panel.
- **`launcher.txt`** — app-dock shortcuts as `Label|program|arguments` (a sample
  template is created on first launch; the right-edge **+** button adds entries
  for you).
- **`rules.txt`** — an `exclude=app.exe` line never captures that exe (commented
  template created on first launch).

> **Update check** (on by default) fetches a small version file from GitHub on
> launch and shows a notification if a newer release exists — notification only,
> nothing is downloaded or installed. Turn it off in Settings.
</details>

## Build

VS 2022 (MSVC v143), Windows SDK 10.0.26100.

```
MSBuild Win32CaptureSample.sln -p:Configuration=Release -p:Platform=x64 -m
```

Output: `x64\Release\SpatialCanvas.exe`. The entire app lives in a single module:
`Win32CaptureSample/Canvas.cpp` (entry point `RunCanvasApp()`).

## License

Source-available, **proprietary** — not open source. Anyone may **download and
use** it for free (personal / educational / internal). **Copying, redistributing,
or offering it commercially is prohibited without written permission.** See
[LICENSE](LICENSE).

## Acknowledgments

The capture infrastructure builds on
[robmikh/Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample) (MIT);
the build uses the `robmikh.common` (MIT, NuGet) package. These third-party
components remain under their respective licenses.

---
© 2026 Batuhan Demirbilek · 13auth
