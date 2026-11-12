# Okular++

A tabbed PDF viewer for Linux with side-by-side panes in one window.
C++20, Qt 6, Poppler.

## Build

Needs `qt6-base`, `poppler-qt6`, `cmake` and `ninja` (Arch package names).

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/okular++ file1.pdf file2.pdf          # one tab per file
./build/okular++ --split left.pdf right.pdf   # start side by side
ctest --test-dir build                        # offscreen smoke test
```

## Keys

| Action | Shortcut |
|---|---|
| Open (new tab) | Ctrl+O, Ctrl+T |
| Close tab | Ctrl+W, middle click |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PgDn / Ctrl+PgUp |
| Split view on/off | F8 |
| Switch to other pane | F6 |
| Move tab to other pane | Ctrl+Shift+M |
| Zoom in / out / 100 % | Ctrl++ / Ctrl+- / Ctrl+0, Ctrl+wheel |
| Fit width / fit page | Ctrl+1 / Ctrl+2 |
| Go to page | Ctrl+G |
| First / last page | Ctrl+Home / Ctrl+End |
| Scroll | wheel, drag, arrows, PgUp/PgDn, Space |

F8 shows the current document in a second pane at the same position; each
pane has its own tabs. Right-click a tab for more options. The pane with the
coloured strip on top is the one keyboard commands apply to.

## Layout

- `src/document.*` — Poppler wrapper shared by all views of one file
- `src/pdfview.*` — continuous-scroll view with background rendering
- `src/tabpane.*` — one tab strip and its views
- `src/mainwindow.*` — the two panes, menus and shortcuts
- `tests/smoketest.cpp` — drives the real window offscreen
