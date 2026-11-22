# yap-dfe

Yet Another P-DF Editor: tabbed, with side-by-side panes in one window,
annotations, search and a dark mode.
C++20, Qt 6, Poppler.

## Build

Needs `qt6-base`, `poppler-qt6`, `qpdf`, `cmake` and `ninja` (Arch package names).

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/yap-dfe file1.pdf file2.pdf          # one tab per file
./build/yap-dfe --split left.pdf right.pdf   # start side by side
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
| Back / forward (after a link or jump) | Alt+Left / Alt+Right, mouse back / forward buttons |
| Scroll | wheel, middle-button drag, drag beside the pages, arrows, PgUp/PgDn, Space |
| Find | Ctrl+F (searches for the selected text, if any) |
| Next / previous match | Enter / Shift+Enter in the find bar, F3 / Shift+F3 |
| Dark mode | Ctrl+D |
| Dark pages in dark mode on/off | Ctrl+Shift+D |

Search covers the whole document, ignores case unless "Match case" is ticked,
and finds words hyphenated across a line end. Dark mode inverts page
lightness but keeps colours, darkens the interface, and is remembered; the
first start follows the desktop's colour scheme. With "Dark Pages in Dark
Mode" switched off (View menu), dark mode only darkens the interface and
leaves the document as it is.

Drag a tab out of its tab bar to move it: onto the other pane, onto a tab
there to take its place, or onto the right half of an unsplit window to
split it. The same works between windows. Dropped outside every window, the
tab moves into a new window of its own. Either way the tab keeps its page and
zoom. A document with unsaved annotations has to be saved before it can move
to another window, since that window reads the file from disk.

Links in the document work with the Select tool: click one in a table of
contents, a reference or a footnote mark to go there, and Alt+Left to come
back. Web links open in the browser; links to anything other than web or
mail addresses ask first. Dragging across a link selects its text as usual.

The status bar shows the page and zoom of the view last clicked or scrolled.
In Fit Width and Fit Page the zoom follows the window size, so it can be
anything, also well over 100 %; 100 % is the printed size.

Started without files, the last window's tabs, split and reading positions
come back. File ▸ Restore Tabs on Startup switches that off.

File ▸ Open Recent lists the last ten files opened, newest first. The same
list is on the start page shown while no file is open; click one, or pick it
with the arrow keys and Enter.

Only one window runs at a time: starting yap-dfe again, e.g. by opening a
PDF from a file manager, adds the files as tabs to the window that is already
open and brings it to the front. `--new-window` opens a separate one instead.

### Annotating

| Tool | Shortcut |
|---|---|
| Select (text, annotations) | Alt+1 |
| Highlight yellow / green / blue / pink | Alt+2 / Alt+3 / Alt+4 / Alt+5 |
| Underline / strike out | Alt+6 / Alt+7 |
| Comment | Alt+8 |
| Text | Alt+9 |
| Pen (red) | Alt+0 |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z |
| Copy selected text | Ctrl+C |
| Select all annotations | Ctrl+A |
| Delete selected annotations | Del |
| Save / save as | Ctrl+S / Ctrl+Shift+S |
| Rotate page clockwise / counterclockwise | Ctrl+R / Ctrl+Shift+R |
| Back to Select | Esc |

- With text selected, Alt+2…7 marks that text. With nothing selected they
  switch to that tool: every drag over text then marks it, until Esc.
- Comment: click where the note should go. Double-click any annotation,
  highlights included, to edit its comment; hover to read it.
- Text: click and type. Enter finishes, Shift+Enter starts a new line, Esc
  cancels. The tool stays on, so several fields can be filled in a row.
- Pen: draw with the left button held. The tool stays on; every stroke is
  an annotation of its own, so Ctrl+Z takes back the last one, and in Select
  a stroke can be clicked (on the line), dragged elsewhere or deleted.
- In Select, click an annotation to select it, drag comments and text to move
  them, right-click for more (recolour, delete).
- Select several: drag a rectangle from blank paper (Shift+drag works over
  text too and adds to the selection), Ctrl+click or Shift+click to add or
  remove one, Ctrl+A for all. Del, dragging and right-click (delete,
  recolour highlights) then act on all of them, and Ctrl+Z undoes it in one
  step.
- Annotations are standard PDF annotations and show up in other viewers.

## Pages

Edit → Remove Pages…, Edit → Rotate Pages… and File → Extract Pages… ask
for pages like `1-3, 5, 8-` (`8-` runs to the last page, `-3` from the
first). Ctrl+R / Ctrl+Shift+R turn the current page clockwise /
counterclockwise. Right-click blank paper on a page to remove, extract or
rotate just that page.

- Removing and rotating pages are edits like any other: Ctrl+Z undoes them,
  and the file on disk changes only when you save. Annotations turn with
  their page.
- Extracting writes the pages, with their annotations (saved or not), to a
  new PDF and leaves the document as it is.
- Password-protected files stay protected with the same password.

F8 shows the current document in a second pane at the same position; each
pane has its own tabs. Right-click a tab for more options. The pane with the
coloured strip on top is the one keyboard commands apply to.

## Layout

- `src/document.*` — Poppler wrapper shared by all views of one file
- `src/pdfview.*` — continuous-scroll view with background rendering
- `src/findbar.*` — the search strip
- `src/tabpane.*` — one tab strip and its views
- `src/startpage.*` — what an empty window shows: the recent files
- `src/mainwindow.*` — the two panes, menus and shortcuts
- `src/singleinstance.*` — hands a second launch's files to the running window
- `tests/smoketest.cpp` — drives the real window offscreen
