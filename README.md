# yap-dfv

Yet Another P-DF Viewer: tabbed, with side-by-side panes in one window,
annotations, search and a dark mode.
C++20, Qt 6, Poppler.

## Build

Needs `qt6-base`, `poppler-qt6`, `cmake` and `ninja` (Arch package names).

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/yap-dfv file1.pdf file2.pdf          # one tab per file
./build/yap-dfv --split left.pdf right.pdf   # start side by side
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
| Find | Ctrl+F (searches for the selected text, if any) |
| Next / previous match | Enter / Shift+Enter in the find bar, F3 / Shift+F3 |
| Dark mode | Ctrl+D |

Search covers the whole document, ignores case unless "Match case" is ticked,
and finds words hyphenated across a line end. Dark mode inverts page
lightness but keeps colours, darkens the interface, and is remembered; the
first start follows the desktop's colour scheme.

### Annotating

| Tool | Shortcut |
|---|---|
| Select (text, annotations) | Alt+1 |
| Highlight yellow / green / blue / pink | Alt+2 / Alt+3 / Alt+4 / Alt+5 |
| Underline / strike out | Alt+6 / Alt+7 |
| Comment | Alt+8 |
| Text | Alt+9 |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z |
| Copy selected text | Ctrl+C |
| Delete selected annotation | Del |
| Save / save as | Ctrl+S / Ctrl+Shift+S |
| Back to Select | Esc |

- With text selected, Alt+2…7 marks that text. With nothing selected they
  switch to that tool: every drag over text then marks it, until Esc.
- Comment: click where the note should go. Double-click any annotation,
  highlights included, to edit its comment; hover to read it.
- Text: click and type. Enter finishes, Shift+Enter starts a new line, Esc
  cancels. The tool stays on, so several fields can be filled in a row.
- In Select, click an annotation to select it, drag comments and text to move
  them, right-click for more (recolour, delete).
- Annotations are standard PDF annotations and show up in other viewers.

F8 shows the current document in a second pane at the same position; each
pane has its own tabs. Right-click a tab for more options. The pane with the
coloured strip on top is the one keyboard commands apply to.

## Layout

- `src/document.*` — Poppler wrapper shared by all views of one file
- `src/pdfview.*` — continuous-scroll view with background rendering
- `src/findbar.*` — the search strip
- `src/tabpane.*` — one tab strip and its views
- `src/mainwindow.*` — the two panes, menus and shortcuts
- `tests/smoketest.cpp` — drives the real window offscreen
