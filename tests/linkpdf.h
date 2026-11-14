#pragma once
#include <QFile>
#include <QStringList>
// A small PDF with links, written by hand: QPdfWriter cannot make links.
// Page 1 (A4) holds four links, one per line of text, from the top:
//   "Chapter Three" -> page 3, halfway down (explicit destination)
//   "Chapter Two"   -> page 2, top (named destination, as LaTeX writes them)
//   "Website"       -> https://example.org/
//   "Last"          -> the Named action LastPage
// Pages 2..4 just say "Page N".
inline bool writeLinkedPdf(const QString &path)
{
    QList<QByteArray> objects; // 1-based object numbers
    const auto add = [&objects](const QByteArray &body) {
        objects.append(body);
        return objects.size();
    };
    const int pages = 4;
    // Fixed numbering: 1 catalog, 2 pages, 3 font, 4.. page/content pairs, then annotations.
    objects.resize(3 + 2 * pages);
    const auto pageObj = [](int i) { return 4 + 2 * i; };
    const auto contentObj = [](int i) { return 5 + 2 * i; };
    const QByteArray mediaBox = "[0 0 595 842]";

    const auto rect = [](int y) { return QByteArray("[70 ") + QByteArray::number(y - 6) + " 260 " + QByteArray::number(y + 20) + "]"; };
    QList<int> annots;
    annots << add("<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect " + rect(710) + " /Dest [" + QByteArray::number(pageObj(2)) + " 0 R /XYZ 0 421 null] >>");
    annots << add("<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect " + rect(660) + " /Dest /chap2 >>");
    annots << add("<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect " + rect(610) + " /A << /S /URI /URI (https://example.org/) >> >>");
    annots << add("<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect " + rect(560) + " /A << /S /Named /N /LastPage >> >>");

    QByteArray kids, annotRefs;
    for (int i = 0; i < pages; ++i) {
        kids += QByteArray::number(pageObj(i)) + " 0 R ";
    }
    for (int a : annots) {
        annotRefs += QByteArray::number(a) + " 0 R ";
    }
    objects[0] = "<< /Type /Catalog /Pages 2 0 R /Dests << /chap2 [" + QByteArray::number(pageObj(1)) + " 0 R /XYZ 0 842 null] >> >>";
    objects[1] = "<< /Type /Pages /Count " + QByteArray::number(pages) + " /Kids [" + kids + "] >>";
    objects[2] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    for (int i = 0; i < pages; ++i) {
        QByteArray content;
        if (i == 0) {
            const QStringList lines{QStringLiteral("Chapter Three"), QStringLiteral("Chapter Two"), QStringLiteral("Website"), QStringLiteral("Last")};
            for (int l = 0; l < lines.size(); ++l) {
                content += "BT /F1 18 Tf 72 " + QByteArray::number(710 - 50 * l) + " Td (" + lines.at(l).toLatin1() + ") Tj ET\n";
            }
        } else {
            content = "BT /F1 18 Tf 72 760 Td (Page " + QByteArray::number(i + 1) + ") Tj ET\n";
        }
        objects[pageObj(i) - 1] = "<< /Type /Page /Parent 2 0 R /MediaBox " + mediaBox + " /Resources << /Font << /F1 3 0 R >> >> /Contents "
            + QByteArray::number(contentObj(i)) + " 0 R" + (i == 0 ? " /Annots [" + annotRefs + "]" : QByteArray()) + " >>";
        objects[contentObj(i) - 1] = "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream";
    }

    QByteArray out = "%PDF-1.4\n";
    QList<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(out.size());
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (qsizetype o : offsets) {
        out += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}
