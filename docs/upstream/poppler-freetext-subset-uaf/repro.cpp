// Touch only the dictionary of each FreeText annotation (set /NM), then save
// with changes. Poppler 26.11 frees the appearance stream mid-use in
// AnnotFreeText::subsetFonts.
//
//   g++ -g -fsanitize=address repro.cpp -o repro $(pkg-config --cflags --libs poppler-qt6 Qt6Gui)
//   python3 make_pdf.py minimal.pdf
//   QT_QPA_PLATFORM=offscreen ./repro minimal.pdf out.pdf
#include <poppler-annotation.h>
#include <poppler-qt6.h>
#include <QGuiApplication>
#include <cstdio>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.pdf out.pdf\n", argv[0]);
        return 2;
    }
    auto doc = Poppler::Document::load(QString::fromLocal8Bit(argv[1]));
    if (!doc) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    auto page = doc->page(0);
    int n = 0;
    for (auto &ann : page->annotations()) {
        ann->setUniqueName(QStringLiteral("name-%1").arg(n++));
    }
    auto converter = doc->pdfConverter();
    converter->setOutputFileName(QString::fromLocal8Bit(argv[2]));
    converter->setPDFOptions(Poppler::PDFConverter::WithChanges);
    const bool ok = converter->convert();
    printf("renamed %d annotations, convert() = %d\n", n, ok);
    return ok ? 0 : 1;
}
