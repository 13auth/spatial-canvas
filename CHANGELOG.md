# Changelog

Spatial Canvas — notable user-facing changes. Versioning is milestone-based
(M = milestone). Dates are 2026.

## 0.52.0 — Connector arrows
- **Connector arrows** — **Ctrl+drag** from one window to another to draw an
  arrow linking them; the line follows the windows as they move. Hover the
  midpoint **✕** to remove. (Session-only.) Together with sticky notes and zone
  frames, the canvas now has the full set of spatial-thinking tools.

## 0.51.0 — Zones group & tidy windows
- Dragging a zone's title bar now **moves the windows inside it** along with it.
- Each zone has a small **grid button** (next to the ✕) that **tidies the
  windows inside it** into a grid — group, then clean up, like a real frame.

## 0.50.0 — Zone frames
- **Zone frames** — press `Ctrl+Shift+Z` to drop a labeled, colored region you
  can use to group windows spatially (FigJam-style). Drag the **title bar** to
  move it; the body is click-through so the windows underneath stay interactive.
  Resize from the corner, rename by double-clicking the title, `Tab` cycles
  color. Persisted across sessions.
- A short "Updated to vX" notice now appears the first time you launch a new
  build (press F1 for shortcuts).

## 0.49.0 — Export canvas to PNG
- **Export your canvas to a PNG** with `Ctrl+Shift+S` (saved to your Pictures
  folder) — handy for sharing a snapshot of your workspace.
- The new-version notice is now clickable — it opens the releases page.
- Fix: the last view is also saved on Windows shutdown/logoff (not just on a
  normal quit), so "Restore last view" no longer loses your spot.

## 0.48.0 — Update notifications, note search, session restore
- **New-version notification** (opt-in, notification-only): the app checks GitHub
  on launch and tells you when a newer release exists — nothing is downloaded or
  installed. Toggle in Settings.
- **Search finds notes too:** `Ctrl+F` now matches sticky-note text, not just
  windows; press Enter to fly to a note.
- **Restore last view:** the canvas reopens exactly where you left off (first
  launch still fits everything). Toggle in Settings.
- Settings panel rows now adapt to screen height (fits short displays).

## 0.47.0 — Bilingual UI (English / Türkçe)
- The entire UI is now **English by default**, with a **language toggle** in
  Settings (English / Türkçe), persisted across sessions.
- README available in English (primary) and Turkish (`README.tr.md`).
- Versioned exe metadata and a proprietary, source-available license.

## 0.46.0 — Canvas notes + release prep
- **Sticky notes (M44–46):** drop notes on the canvas with `Ctrl+Shift+N`;
  color (Tab), drag, double-click to edit, **resize** from the bottom-right
  corner, delete via hover-`✕`. Notes persist in `notes.txt`; integrated with
  fit / minimap / F1. IPC: `note:text`.
- Single-file distribution (no redistributable), versioned exe stamp + icon.

## 0.42–0.45 — Workspaces + discoverability
- **Named workspaces (M42):** save/load layout profiles (`save <name>` /
  `load <name>`, IPC `save:`/`load:`).
- Exe icon on tile labels (M41); F1 list + empty-canvas onboarding hint.

## 0.40.0 — Autonomous development wave (M33–M40)
- Sharp dock icons (App Paths + JUMBO), per-window blur rule (`rules.txt`).
- `Ctrl+G` arrange into a grid (Miro clean-up style).
- **Minimap** (bottom-right bird's-eye; viewport + bookmark dots; click-to-jump).
- Hover label (zoom-independent), `Shift+Arrow` to move the focused tile.
- Command palette expanded (`Ctrl+N` accepts fit/grid/quit).

## 0.30–0.34 — Competitive parity (M27–M34)
- Pinned-tile persistence, per-window **opacity** rule (alpha blend), **vignette**
  background.
- **IPC / scripting** (named pipe: fit/quit/pull/launch/search/bookmark).
- Horizontal touchpad pan; box-blur window rule.

## 0.16–0.26 — Resilience + UX (M16–M26)
- Single instance, crash insurance, full device-lost reinit, live resolution/DPI.
- Toast HUD, ESC context ladder, first-launch card + **F1** shortcut list.
- Keyboard navigation (arrows/Enter/Tab-MRU), screen-pinned HUD tile (`Ctrl+P`).
- App launcher (`Ctrl+N` palette + right-edge dock with real icons, CRUD).
- Performance: grid bitmap brush, brush cache, idle GPU throttling.

## 0.11–0.15 — Canvas shell (M11–M15)
- Taskbar hiding, top-center Search button, `Ctrl+C/V` duplication.
- Marquee multi-select + Delete; momentum pan, edge auto-pan, dot grid.
- Bookmarks (`Ctrl+Shift+1–4` / `Ctrl+1–4`), bottom-edge dock, edge snapping,
  window rules (`rules.txt`).

## 0.1–0.10 — Core (M0–M10)
- WGC capture → D3D11 texture; infinite zoom/pan canvas; **park & swap** (dive
  into the real HWND, no input simulation); customizable shortcuts.
