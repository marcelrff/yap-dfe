# Draft: heap-use-after-free in `AnnotFreeText::subsetFonts` on save

Status: **draft, not yet filed** at <https://gitlab.freedesktop.org/poppler/poppler/-/issues>.

---

## Summary

Saving a document with changes (`PDFDoc::saveAs` → `saveIncrementalUpdate`) can
use freed memory in `AnnotFreeText::subsetFonts`. It happens when a FreeText
annotation was marked updated while its appearance stream is still the original
object from the file. A real application crashes in `malloc` later (SIGSEGV or
SIGABRT, heap corruption). Under AddressSanitizer it reports straight away as a
heap-use-after-free.

## Version

- poppler 26.11.0 (Arch Linux `poppler 26.11.0-1`, `poppler-qt6 26.11.0-1`)
- Reached from the Qt6 frontend through `Poppler::PDFConverter::convert()` with
  `PDFConverter::WithChanges`.

## Steps to reproduce

Files in this directory:

- `make_pdf.py`: writes a minimal one-page PDF with two FreeText annotations. They
  have no `/NM`, and their `/AP /N` is a normal form XObject with a `/Font` resource.
- `repro.cpp`: loads the PDF, sets a unique name on each annotation (this only
  changes the annotation dictionary), and saves with changes.

```sh
g++ -g -fsanitize=address repro.cpp -o repro $(pkg-config --cflags --libs poppler-qt6 Qt6Gui)
python3 make_pdf.py minimal.pdf
QT_QPA_PLATFORM=offscreen ./repro minimal.pdf out.pdf
```

## Actual result

```
==ERROR: AddressSanitizer: heap-use-after-free ...
READ of size 9 at ... thread T0
    #1 memcmp
    #2 (libpoppler.so.166)
    #3 Dict::set(std::basic_string_view<char>, Object&&)
    #4 AnnotFreeText::subsetFonts(FontSubsetter const*)
    #5 FontSubsetter::subsetAll() (FontSubsetter.cc:218)
    #6 PDFDoc::saveIncrementalUpdate(OutStream*)
    #7 PDFDoc::saveAs(OutStream*, PDFWriteMode)
    #8 Poppler::PDFConverter::convert()

freed by thread T0 here:
    #0 operator delete(void*, unsigned long)
    ...
    #3 FileStream::~FileStream()
    ...
    #6 AnnotFreeText::subsetFonts(FontSubsetter const*)
```

Without ASan the heap is quietly corrupted. The crash comes on a later allocation
in the same loop, for example inside `Parser::getObj` / `XRef::fetch` while the
next FreeText annotation is processed.

## Expected result

The document is saved, and the fonts of the updated FreeText annotations are
subset or left alone.

## Analysis

`poppler/Annot.cc`, at the end of `AnnotFreeText::subsetFonts` (line 3421 in 26.11.0):

```cpp
Dict *apDict = appearance.fetch(doc->getXRef()).getStream()->getDict();
apDict->set("Resources", std::move(resourcesCopy));
```

`appearance.fetch()` returns a temporary `Object`. When the appearance stream is
read from the file, that temporary is the only owner of the newly parsed `Stream`.
The temporary is destroyed at the end of the first statement, so `apDict` points
into the freed stream, and `Dict::set` on the next line writes to freed memory.

Keeping the fetched object alive would avoid the use-after-free:

```cpp
Object apObj = appearance.fetch(doc->getXRef());
Dict *apDict = apObj.getStream()->getDict();
apDict->set("Resources", std::move(resourcesCopy));
```

That is probably not the whole fix, though. The modified dictionary belongs to a
copy that was fetched just for this, so the new `/Resources` would only reach the
output if the stream is also written back. Something like
`doc->getXRef()->setModifiedObject(&apObj, appearance.getRef())` would do that,
for a referenced stream. I haven't checked how the rest of the subsetting path
expects this to work.

When the appearance was regenerated in the same session (for example after
`setBoundary`, `setContents` or `setTextFont`), the stream already lives in memory
and the fetch shares ownership with it. In that case the bug doesn't show, which
is why it is easy to miss. The trigger is a change that touches only the
dictionary, such as setting `/NM`, flags or the modification date.

## Impact

Any application that names or flags existing FreeText annotations without
redrawing them, then saves, can crash or corrupt its heap. That includes
annotations that other PDF tools created without `/NM`.

## How yap-dfe works around it

yap-dfe used to give every unnamed annotation a name when it read a page, which
hit this bug on every save. It now addresses unnamed annotations by an id held
only in memory and writes `/NM` only when it edits an annotation, which always
redraws it too (see `annotationId()` in `src/document.cpp`).
