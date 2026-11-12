// Drives the real main window offscreen: tabs, split view, zoom, navigation
// and rendering, against PDFs generated on the fly.

#include "document.h"
#include "mainwindow.h"
#include "pdfview.h"
#include "tabpane.h"

#include <QAction>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>

class SmokeTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_a, m_b;

    static void writePdf(const QString &path, int pages, const QString &label)
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter p(&writer);
        QFont font = p.font();
        font.setPointSize(48);
        p.setFont(font);
        for (int i = 0; i < pages; ++i) {
            if (i > 0) {
                writer.newPage();
            }
            p.fillRect(QRect(0, 0, writer.width(), writer.height() / 4), Qt::darkBlue);
            p.drawText(QRect(0, 0, writer.width(), writer.height()), Qt::AlignCenter, QStringLiteral("%1 %2").arg(label).arg(i + 1));
        }
    }

    static QAction *action(MainWindow &w, const QString &text)
    {
        for (QAction *a : w.findChildren<QAction *>()) {
            if (a->text().remove(QLatin1Char('&')) == text) {
                return a;
            }
        }
        return nullptr;
    }

    static void trigger(MainWindow &w, const QString &text)
    {
        QAction *a = action(w, text);
        QVERIFY2(a, qPrintable(text));
        QVERIFY2(a->isEnabled(), qPrintable(text));
        a->trigger();
    }

    struct Fixture {
        MainWindow window;
        TabPane *left;
        TabPane *right;
        Fixture()
        {
            left = window.findChild<TabPane *>(QStringLiteral("leftPane"));
            right = window.findChild<TabPane *>(QStringLiteral("rightPane"));
            window.show();
            (void)QTest::qWaitForWindowExposed(&window);
        }
    };

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_a = m_dir.filePath(QStringLiteral("a.pdf"));
        m_b = m_dir.filePath(QStringLiteral("b.pdf"));
        writePdf(m_a, 6, QStringLiteral("A"));
        writePdf(m_b, 3, QStringLiteral("B"));
    }

    void loadStatuses()
    {
        Document::LoadStatus status;
        const auto doc = Document::load(m_a, QString(), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        QCOMPARE(doc->pageCount(), 6);
        QVERIFY(qAbs(doc->pageSize(0).width() - 595) < 2);

        const QString bad = m_dir.filePath(QStringLiteral("bad.pdf"));
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not a pdf");
        f.close();
        QVERIFY(!Document::load(bad, QString(), &status));
        QCOMPARE(status, Document::LoadStatus::Failed);
    }

    void encrypted()
    {
        const QString qpdf = QStandardPaths::findExecutable(QStringLiteral("qpdf"));
        if (qpdf.isEmpty()) {
            QSKIP("qpdf not installed");
        }
        const QString enc = m_dir.filePath(QStringLiteral("enc.pdf"));
        QCOMPARE(QProcess::execute(qpdf, {QStringLiteral("--encrypt"), QStringLiteral("secret"), QStringLiteral("secret"),
                                          QStringLiteral("256"), QStringLiteral("--"), m_a, enc}),
                 0);
        Document::LoadStatus status;
        QVERIFY(!Document::load(enc, QString(), &status));
        QCOMPARE(status, Document::LoadStatus::Locked);
        QVERIFY(!Document::load(enc, QStringLiteral("wrong"), &status));
        QCOMPARE(status, Document::LoadStatus::Locked);
        const auto doc = Document::load(enc, QStringLiteral("secret"), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        QVERIFY(!doc->render(0, QSize(200, 283)).isNull());
    }

    void tabs()
    {
        Fixture f;
        QVERIFY(!action(f.window, QStringLiteral("Close Tab"))->isEnabled());
        f.window.openFiles({m_a, m_b});
        QCOMPARE(f.left->count(), 2);
        QCOMPARE(f.left->currentIndex(), 1);
        QVERIFY(f.window.windowTitle().startsWith(QStringLiteral("b.pdf")));

        // Opening an already open file switches to its tab instead of duplicating it.
        f.window.openFile(m_a);
        QCOMPARE(f.left->count(), 2);
        QCOMPARE(f.left->currentIndex(), 0);

        trigger(f.window, QStringLiteral("Next Tab"));
        QCOMPARE(f.left->currentIndex(), 1);
        trigger(f.window, QStringLiteral("Next Tab"));
        QCOMPARE(f.left->currentIndex(), 0);
        trigger(f.window, QStringLiteral("Previous Tab"));
        QCOMPARE(f.left->currentIndex(), 1);

        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.left->currentView()->document()->fileName(), QStringLiteral("a.pdf"));

        // Middle click closes.
        QTest::mouseClick(f.left->tabBar(), Qt::MiddleButton, {}, f.left->tabBar()->tabRect(0).center());
        QCOMPARE(f.left->count(), 0);
        QCOMPARE(f.window.windowTitle(), QStringLiteral("Okular++"));
    }

    void renderingAndNavigation()
    {
        Fixture f;
        f.window.openFile(m_a);
        PdfView *v = f.left->currentView();
        QTRY_VERIFY(!f.window.isRendering());

        // The dark blue band at the top of the page must be on screen.
        const QImage shot = v->viewport()->grab().toImage();
        QCOMPARE(shot.pixelColor(shot.width() / 2, 40), QColor(Qt::darkBlue));

        QCOMPARE(v->zoomMode(), PdfView::ZoomMode::FitWidth);
        QCOMPARE(v->currentPage(), 0);
        trigger(f.window, QStringLiteral("Last Page"));
        QCOMPARE(v->currentPage(), 5);
        trigger(f.window, QStringLiteral("First Page"));
        QCOMPARE(v->currentPage(), 0);

        const double before = v->zoom();
        trigger(f.window, QStringLiteral("Zoom In"));
        QVERIFY(v->zoom() > before);
        QCOMPARE(v->zoomMode(), PdfView::ZoomMode::Custom);
        trigger(f.window, QStringLiteral("Actual Size"));
        QCOMPARE(v->zoom(), 1.0);
        trigger(f.window, QStringLiteral("Fit Page"));
        QCOMPARE(v->zoomMode(), PdfView::ZoomMode::FitPage);
        QVERIFY(action(f.window, QStringLiteral("Fit Page"))->isChecked());

        // Zooming keeps the reading position.
        v->goToPage(3);
        for (int i = 0; i < 5; ++i) {
            trigger(f.window, QStringLiteral("Zoom In"));
        }
        QCOMPARE(v->currentPage(), 3);
        QTRY_VERIFY(!f.window.isRendering());
    }

    void splitView()
    {
        Fixture f;
        f.window.openFiles({m_a, m_b});
        f.left->setCurrentIndex(0);
        PdfView *a = f.left->currentView();
        a->goToPage(4);

        // Split shows the same document, at the same place, in the right pane.
        trigger(f.window, QStringLiteral("Split View"));
        QCOMPARE(f.left->count(), 2);
        QCOMPARE(f.right->count(), 1);
        QVERIFY(f.right->isVisible());
        PdfView *copy = f.right->currentView();
        QCOMPARE(copy->document(), a->document()); // shared, not loaded twice
        QCOMPARE(copy->currentPage(), 4);
        QVERIFY(qAbs(f.left->width() - f.right->width()) < 20);
        QTRY_VERIFY(!f.window.isRendering());

        // The two halves scroll independently.
        copy->goToPage(1);
        QCOMPARE(copy->currentPage(), 1);
        QCOMPARE(a->currentPage(), 4);

        // The right pane is active now, so new files open there.
        f.window.openFile(m_b);
        QCOMPARE(f.right->count(), 2);
        QCOMPARE(f.left->count(), 2);

        // Switch panes and move b.pdf's left tab across: it joins the right pane.
        trigger(f.window, QStringLiteral("Switch to Other Pane"));
        f.left->setCurrentIndex(1);
        trigger(f.window, QStringLiteral("Move Tab to Other Pane"));
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.right->count(), 3);

        // Closing the last left tab promotes the right pane's tabs.
        f.window.openFile(m_a, 0);
        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(f.left->count(), 3);
        QCOMPARE(f.right->count(), 0);
        QVERIFY(!f.right->isVisible());
        QVERIFY(!action(f.window, QStringLiteral("Split View"))->isChecked());

        // Split again, then un-split: everything merges back to the left.
        trigger(f.window, QStringLiteral("Split View"));
        QCOMPARE(f.right->count(), 1);
        trigger(f.window, QStringLiteral("Split View"));
        QCOMPARE(f.right->count(), 0);
        QCOMPARE(f.left->count(), 4);
        QVERIFY(!f.right->isVisible());

        // Close everything.
        while (f.left->count() > 0) {
            trigger(f.window, QStringLiteral("Close Tab"));
        }
        QVERIFY(!action(f.window, QStringLiteral("Split View"))->isEnabled());
    }

    void moveOnlyTabIsNoOp()
    {
        Fixture f;
        f.window.openFile(m_a);
        trigger(f.window, QStringLiteral("Move Tab to Other Pane"));
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.right->count(), 0);
    }
};

QTEST_MAIN(SmokeTest)
#include "smoketest.moc"
