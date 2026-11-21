// Drives the real main window offscreen: tabs, split view, zoom, navigation
// and rendering, against PDFs generated on the fly.

#include "document.h"
#include "mainwindow.h"
#include "pdfview.h"
#include "singleinstance.h"
#include "startpage.h"
#include "tabpane.h"

#include <QAction>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QClipboard>
#include <QPlainTextEdit>
#include <QPainter>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMessageBox>
#include <QPdfWriter>
#include <QProcess>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QStatusBar>
#include <QWheelEvent>

#include "linkpdf.h"

class SmokeTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_a, m_b, m_links;

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

    // The payload of a tab drag, as a window would make it.
    static QByteArray tabPayload(const MainWindow &w, int pane, int index)
    {
        return QJsonDocument(QJsonObject{{QStringLiteral("window"), w.windowId()},
                                         {QStringLiteral("pane"), pane},
                                         {QStringLiteral("index"), index}})
            .toJson(QJsonDocument::Compact);
    }

    // Plays a drag over `target` and, if it accepts, the drop. Returns what a
    // real drag would report back to the source.
    static Qt::DropAction dropOn(QWidget *target, const QMimeData *mime, const QPoint &pos)
    {
        QDragEnterEvent enter(QPointF(pos), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent moveEvent(QPointF(pos), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &moveEvent);
        if (!moveEvent.isAccepted()) {
            QDragLeaveEvent leave;
            QApplication::sendEvent(target, &leave);
            return Qt::IgnoreAction;
        }
        QDropEvent dropEvent(QPointF(pos), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &dropEvent);
        return dropEvent.isAccepted() ? dropEvent.dropAction() : Qt::IgnoreAction;
    }

    // Drags a tab out of its bar with the mouse, which starts a real drag.
    static void dragTabOut(TabPane *pane, int index)
    {
        QTabBar *bar = pane->tabBar();
        const QPoint onTab = bar->tabRect(index).center();
        QTest::mousePress(bar, Qt::LeftButton, {}, onTab);
        // QTest's own move helper sends no button state, which a drag needs.
        const QPoint away = onTab + QPoint(40, 200);
        QMouseEvent event(QEvent::MouseMove, away, bar->mapToGlobal(away), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &event);
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


    // Scrolls the page label to about a third down the viewport, unless it
    // is already comfortably in view.
    static void showLabel(PdfView *v, int page)
    {
        const auto text = v->document()->pageText(page);
        const int y = v->viewportPoint(page, text->first().rect.topLeft()).y();
        if (y < 20 || y > v->viewport()->height() - 150) {
            QScrollBar *bar = v->verticalScrollBar();
            bar->setValue(bar->value() + y - v->viewport()->height() / 3);
        }
    }

    // Drags across the page label ("A 1") with the left button.
    static void dragAcrossText(PdfView *v, int page)
    {
        const auto text = v->document()->pageText(page);
        QVERIFY(text->size() >= 2);
        showLabel(v, page);
        QVERIFY(v->viewport()->rect().contains(v->viewportPoint(page, text->last().rect.center())));
        const QPoint from = v->viewportPoint(page, text->first().rect.center());
        const QPoint to = v->viewportPoint(page, text->last().rect.center());
        QTest::mousePress(v->viewport(), Qt::LeftButton, {}, from);
        QTest::mouseMove(v->viewport(), (from + to) / 2);
        QTest::mouseMove(v->viewport(), to);
        QTest::mouseRelease(v->viewport(), Qt::LeftButton, {}, to);
    }

    // Colour of the page background just inside the first character's box.
    static QColor colorAtText(PdfView *v, int page)
    {
        showLabel(v, page);
        const auto text = v->document()->pageText(page);
        const QPoint p = v->viewportPoint(page, text->first().rect.topLeft()) + QPoint(3, 3);
        return v->viewport()->grab().toImage().pixelColor(p);
    }

    static bool isYellow(const QColor &c)
    {
        return c.red() > 220 && c.green() > 200 && c.blue() < 150;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // Keep the test's settings (dark mode) away from the user's.
        qputenv("XDG_CONFIG_HOME", m_dir.path().toUtf8());
        m_a = m_dir.filePath(QStringLiteral("a.pdf"));
        m_b = m_dir.filePath(QStringLiteral("b.pdf"));
        writePdf(m_a, 6, QStringLiteral("A"));
        writePdf(m_b, 3, QStringLiteral("B"));
        m_links = m_dir.filePath(QStringLiteral("links.pdf"));
        QVERIFY(writeLinkedPdf(m_links));
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

        // Page operations work on it too, and their output stays encrypted.
        QString error;
        QVERIFY2(doc->removePages({0}, &error), qPrintable(error));
        QCOMPARE(doc->pageCount(), 5);
        QVERIFY(!doc->render(0, QSize(200, 283)).isNull());
        const QString part = m_dir.filePath(QStringLiteral("enc-part.pdf"));
        QVERIFY2(doc->extractPages({0, 1}, part, &error), qPrintable(error));
        QVERIFY(!Document::load(part, QString(), &status));
        QCOMPARE(status, Document::LoadStatus::Locked);
        const auto partDoc = Document::load(part, QStringLiteral("secret"), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        QCOMPARE(partDoc->pageCount(), 2);
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
        QCOMPARE(f.window.windowTitle(), QStringLiteral("yap-dfe"));
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

    // Closing a tab reveals its neighbour. If that one was last laid out at a
    // different window size it resizes, and so re-zooms, in the middle of the
    // removal, while the pane's tab bar and page stack disagree.
    void closeTabRevealingStaleTab()
    {
        Fixture f;
        f.window.openFiles({m_a, m_b, m_a});
        QCOMPARE(f.left->count(), 2);
        f.left->setCurrentIndex(0);
        trigger(f.window, QStringLiteral("Fit Width"));
        f.left->setCurrentIndex(1);
        f.window.resize(f.window.width() - 300, f.window.height());
        QCoreApplication::processEvents();

        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.left->tabText(0), QStringLiteral("a.pdf"));
        QVERIFY(f.window.windowTitle().startsWith(QStringLiteral("a.pdf")));
    }

    // A second launch hands its files to the first one's window.
    void secondLaunchOpensTabs()
    {
        Fixture f;
        f.window.openFile(m_a);
        const QString base = m_dir.filePath(QStringLiteral("instance"));
        SingleInstance first(base);
        QVERIFY(first.claim());
        connect(&first, &SingleInstance::requestReceived, &f.window, [&](const SingleInstance::Request &request) {
            f.window.openRequest(request.files, request.split);
        });

        SingleInstance second(base);
        QVERIFY(!second.claim());
        QVERIFY(second.send({{m_b}, false, {}}));
        QTRY_COMPARE(f.left->count(), 2);
        QCOMPARE(f.left->currentView()->document()->fileName(), QStringLiteral("b.pdf"));
        QCOMPARE(f.right->count(), 0);

        // A file that is already open is switched to, not opened twice.
        QVERIFY(second.send({{m_a}, false, {}}));
        QTRY_COMPARE(f.left->currentIndex(), 0);
        QCOMPARE(f.left->count(), 2);

        // --split puts the last file in the right pane.
        QVERIFY(second.send({{m_a, m_b}, true, {}}));
        QTRY_COMPARE(f.right->count(), 1);
        QCOMPARE(f.right->currentView()->document()->fileName(), QStringLiteral("b.pdf"));
        QCOMPARE(f.left->count(), 2);
    }

    // With nobody listening the launch must notice and open its own window.
    void sendWithoutListenerFails()
    {
        const SingleInstance lonely(m_dir.filePath(QStringLiteral("nobody")));
        QVERIFY(!lonely.send({{m_a}, false, {}}, 100));
    }

    void moveOnlyTabIsNoOp()
    {
        Fixture f;
        f.window.openFile(m_a);
        trigger(f.window, QStringLiteral("Move Tab to Other Pane"));
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.right->count(), 0);
    }
    void annotate()
    {
        Fixture f;
        f.window.openFile(m_a);
        PdfView *v = f.left->currentView();
        const std::shared_ptr<Document> doc = v->document();
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(!action(f.window, QStringLiteral("Undo"))->isEnabled());
        QVERIFY(!action(f.window, QStringLiteral("Save"))->isEnabled());

        // Select text with the mouse and copy it.
        dragAcrossText(v, 0);
        QCOMPARE(v->selectedText(), QStringLiteral("A 1"));
        trigger(f.window, QStringLiteral("Copy"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("A 1"));

        // A highlight shortcut marks the selection without changing the tool.
        QCOMPARE(action(f.window, QStringLiteral("Highlight Yellow"))->shortcut(), QKeySequence(Qt::ALT | Qt::Key_2));
        trigger(f.window, QStringLiteral("Highlight Yellow"));
        QCOMPARE(f.window.tool(), PdfView::Tool::Select);
        QVERIFY(action(f.window, QStringLiteral("Select"))->isChecked());
        QVERIFY(!v->hasTextSelection());
        QCOMPARE(doc->annotations(0).size(), 1);
        QCOMPARE(doc->annotations(0).first().kind, AnnotationData::Kind::Highlight);
        QVERIFY(isYellow(doc->annotations(0).first().color));
        QVERIFY(doc->isModified());
        QCOMPARE(f.left->tabText(0), QStringLiteral("a.pdf *"));
        QVERIFY(isYellow(colorAtText(v, 0))); // shown immediately...
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(isYellow(colorAtText(v, 0))); // ...and in the re-rendered page

        // Undo and redo.
        trigger(f.window, QStringLiteral("Undo"));
        QCOMPARE(doc->annotations(0).size(), 0);
        QVERIFY(!doc->isModified());
        QTRY_VERIFY(!f.window.isRendering());
        QCOMPARE(colorAtText(v, 0), QColor(Qt::white));
        trigger(f.window, QStringLiteral("Redo"));
        QCOMPARE(doc->annotations(0).size(), 1);

        // With nothing selected the shortcut picks the tool, which stays on.
        v->goToPage(1);
        trigger(f.window, QStringLiteral("Highlight Green"));
        QCOMPARE(f.window.tool(), PdfView::Tool::Highlight);
        dragAcrossText(v, 1);
        QCOMPARE(doc->annotations(1).size(), 1);
        QVERIFY(doc->annotations(1).first().color.green() > doc->annotations(1).first().color.red());
        QCOMPARE(f.window.tool(), PdfView::Tool::Highlight);
        trigger(f.window, QStringLiteral("Underline"));
        dragAcrossText(v, 1);
        QCOMPARE(doc->annotations(1).size(), 2);
        QCOMPARE(doc->annotations(1).last().kind, AnnotationData::Kind::Underline);
        QTest::keyClick(v, Qt::Key_Escape);
        QCOMPARE(f.window.tool(), PdfView::Tool::Select);
        v->goToPage(0);

        // Comment: click to place, type in the (stubbed) dialog.
        const auto savedEditor = PdfView::commentEditor;
        PdfView::commentEditor = [](QWidget *, QString *text) {
            *text = QStringLiteral("Hello");
            return true;
        };
        trigger(f.window, QStringLiteral("Comment"));
        const QPoint notePos = v->viewportPoint(0, QPointF(0.2, 0.08));
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, notePos);
        QCOMPARE(doc->annotations(0).size(), 2);
        QCOMPARE(doc->annotations(0).last().kind, AnnotationData::Kind::Note);
        QCOMPARE(doc->annotations(0).last().contents, QStringLiteral("Hello"));
        QCOMPARE(f.window.tool(), PdfView::Tool::Select); // one comment per activation
        QCOMPARE(v->selectedAnnotation(), doc->annotations(0).last().id);

        // Double-click edits the comment; highlights can carry one too.
        PdfView::commentEditor = [](QWidget *, QString *text) {
            *text += QStringLiteral(" again");
            return true;
        };
        QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, notePos);
        QCOMPARE(doc->annotations(0).last().contents, QStringLiteral("Hello again"));
        PdfView::commentEditor = savedEditor;

        // Delete the selected comment.
        trigger(f.window, QStringLiteral("Delete Annotations"));
        QCOMPARE(doc->annotations(0).size(), 1);
        QVERIFY(!action(f.window, QStringLiteral("Delete Annotations"))->isEnabled());

        // Text: click, type, Enter.
        trigger(f.window, QStringLiteral("Text"));
        const QPoint textPos = v->viewportPoint(0, QPointF(0.3, 0.2));
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, textPos);
        auto *editor = v->viewport()->findChild<QPlainTextEdit *>();
        QVERIFY(editor);
        editor->insertPlainText(QStringLiteral("Größe ")); // QTest cannot type non-ASCII keys
        QTest::keyClicks(editor, QStringLiteral("12"));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(doc->annotations(0).size(), 2);
        AnnotationData typed = doc->annotations(0).last();
        QCOMPARE(typed.kind, AnnotationData::Kind::Text);
        QCOMPARE(typed.contents, QStringLiteral("Größe 12"));
        QVERIFY(qAbs(typed.boundary.x() - 0.3) < 0.01);
        QCOMPARE(f.window.tool(), PdfView::Tool::Text); // stays on for the next field
        QTest::keyClick(v, Qt::Key_Escape);
        QTest::keyClick(v, Qt::Key_Escape);
        QCOMPARE(f.window.tool(), PdfView::Tool::Select);

        // The typed text is really drawn: dark pixels inside its box.
        QTRY_VERIFY(!f.window.isRendering());
        {
            const QImage shot = v->viewport()->grab().toImage();
            const QRect box(v->viewportPoint(0, typed.boundary.topLeft()), v->viewportPoint(0, typed.boundary.bottomRight()));
            int dark = 0;
            for (int y = box.top(); y <= box.bottom(); ++y) {
                for (int x = box.left(); x <= box.right(); ++x) {
                    dark += shot.pixelColor(x, y).lightness() < 100;
                }
            }
            QVERIFY2(dark > 20, qPrintable(QString::number(dark)));
        }

        // Drag the text somewhere else.
        const QPoint grabPos = v->viewportPoint(0, typed.boundary.center());
        QTest::mousePress(v->viewport(), Qt::LeftButton, {}, grabPos);
        QTest::mouseMove(v->viewport(), grabPos + QPoint(30, 20));
        QTest::mouseMove(v->viewport(), grabPos + QPoint(60, 40));
        QTest::mouseRelease(v->viewport(), Qt::LeftButton, {}, grabPos + QPoint(60, 40));
        const AnnotationData moved = doc->annotations(0).last();
        QCOMPARE(moved.id, typed.id);
        QVERIFY(moved.boundary.x() > typed.boundary.x() + 0.02);
        QVERIFY(moved.boundary.y() > typed.boundary.y() + 0.01);
        QCOMPARE(moved.boundary.size(), typed.boundary.size());

        // Double-click re-opens the text for editing; emptying it deletes it.
        QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, v->viewportPoint(0, moved.boundary.center()));
        editor = v->viewport()->findChild<QPlainTextEdit *>();
        QVERIFY(editor);
        QCOMPARE(editor->toPlainText(), QStringLiteral("Größe 12"));
        QTest::keyClicks(editor, QStringLiteral("!"));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(doc->annotations(0).last().contents, QStringLiteral("Größe 12!"));
    }

    void saveAndReload()
    {
        Fixture f;
        const QString work = m_dir.filePath(QStringLiteral("work.pdf"));
        QVERIFY(QFile::copy(m_a, work));
        f.window.openFile(work);
        PdfView *v = f.left->currentView();
        const std::shared_ptr<Document> doc = v->document();

        dragAcrossText(v, 0);
        trigger(f.window, QStringLiteral("Highlight Pink"));
        dragAcrossText(v, 0);
        trigger(f.window, QStringLiteral("Strike Out"));
        QCOMPARE(doc->annotations(0).size(), 2);

        // Save in place.
        trigger(f.window, QStringLiteral("Save"));
        QVERIFY(!doc->isModified());
        QCOMPARE(f.left->tabText(0), QStringLiteral("work.pdf"));
        QVERIFY(!action(f.window, QStringLiteral("Save"))->isEnabled());
        QCOMPARE(QDir(m_dir.path()).entryList({QStringLiteral(".work.pdf*")}, QDir::Files | QDir::Hidden).size(), 0); // no temp file left

        Document::LoadStatus status;
        {
            const auto reloaded = Document::load(work, QString(), &status);
            QCOMPARE(status, Document::LoadStatus::Ok);
            const QList<AnnotationData> list = reloaded->annotations(0);
            QCOMPARE(list.size(), 2);
            QCOMPARE(list.at(0).kind, AnnotationData::Kind::Highlight);
            QCOMPARE(list.at(1).kind, AnnotationData::Kind::StrikeOut);
            QCOMPARE(list.at(0).id, doc->annotations(0).at(0).id);
            QVERIFY(qAbs(list.at(0).boundary.x() - doc->annotations(0).at(0).boundary.x()) < 0.001);
            QCOMPARE(reloaded->pageCount(), 6);
        }

        // Keep working after the save, then save again: the open document
        // must survive having its file replaced underneath it.
        AnnotationData note;
        note.kind = AnnotationData::Kind::Note;
        note.color = PdfView::noteColor();
        note.boundary = QRectF(0.5, 0.5, 0.03, 0.02);
        note.contents = QStringLiteral("after first save");
        doc->addAnnotation(note);
        doc->removeAnnotation(0, doc->annotations(0).first().id);
        QVERIFY(doc->isModified());
        trigger(f.window, QStringLiteral("Save"));
        {
            const auto reloaded = Document::load(work, QString(), &status);
            QCOMPARE(status, Document::LoadStatus::Ok);
            const QList<AnnotationData> list = reloaded->annotations(0);
            QCOMPARE(list.size(), 2);
            QCOMPARE(list.at(0).kind, AnnotationData::Kind::StrikeOut);
            QCOMPARE(list.at(1).contents, QStringLiteral("after first save"));
            QVERIFY(!reloaded->render(0, QSize(300, 424)).isNull());
        }

        // Undoing past the save point makes the document modified again.
        trigger(f.window, QStringLiteral("Undo"));
        QVERIFY(doc->isModified());
        trigger(f.window, QStringLiteral("Redo"));
        QVERIFY(!doc->isModified());

        // Saving under a new name renames the tab.
        const QString copy = m_dir.filePath(QStringLiteral("copy.pdf"));
        QString error;
        QVERIFY2(doc->save(copy, &error), qPrintable(error));
        QCOMPARE(f.left->tabText(0), QStringLiteral("copy.pdf"));
        QVERIFY(QFile::exists(copy));
        QVERIFY(QFile::permissions(copy) & QFile::ReadGroup);
    }

    void unsavedChangesPrompt()
    {
        Fixture f;
        const QString work = m_dir.filePath(QStringLiteral("prompt.pdf"));
        QVERIFY(QFile::copy(m_a, work));
        f.window.openFile(work);
        PdfView *v = f.left->currentView();

        int asked = 0;
        QMessageBox::StandardButton answer = QMessageBox::Cancel;
        f.window.unsavedPrompt = [&](const QString &) {
            ++asked;
            return answer;
        };

        // Nothing changed: no question.
        f.window.openFile(m_b);
        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(asked, 0);
        QCOMPARE(f.left->count(), 1);

        dragAcrossText(v, 0);
        trigger(f.window, QStringLiteral("Highlight Blue"));

        // A second view of the same document can close freely.
        trigger(f.window, QStringLiteral("Split View"));
        QCOMPARE(f.right->count(), 1);
        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(asked, 0);
        QCOMPARE(f.right->count(), 0);

        // The last one asks. Cancel keeps it, also for the whole window.
        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(asked, 1);
        QCOMPARE(f.left->count(), 1);
        QVERIFY(!f.window.close());
        QCOMPARE(asked, 2);
        QVERIFY(f.window.isVisible());

        // Save writes the file and closes.
        answer = QMessageBox::Save;
        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(f.left->count(), 0);
        Document::LoadStatus status;
        QCOMPARE(Document::load(work, QString(), &status)->annotations(0).size(), 1);

        // Discard closes without writing.
        f.window.openFile(work);
        v = f.left->currentView();
        v->document()->removeAnnotation(0, v->document()->annotations(0).first().id);
        answer = QMessageBox::Discard;
        trigger(f.window, QStringLiteral("Close Tab"));
        QCOMPARE(f.left->count(), 0);
        QCOMPARE(Document::load(work, QString(), &status)->annotations(0).size(), 1);
    }

    void splitViewsStayInSync()
    {
        Fixture f;
        f.window.openFile(m_a);
        trigger(f.window, QStringLiteral("Split View"));
        PdfView *left = f.left->currentView();
        PdfView *right = f.right->currentView();
        QTRY_VERIFY(!f.window.isRendering());
        QCOMPARE(colorAtText(right, 0), QColor(Qt::white));

        // Highlight in the left pane; the right pane shows it too.
        dragAcrossText(left, 0);
        QVERIFY(left->hasTextSelection());
        trigger(f.window, QStringLiteral("Highlight Yellow")); // clicking in the left pane made it active
        QCOMPARE(left->document()->annotations(0).size(), 1);
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(isYellow(colorAtText(right, 0)));
        QCOMPARE(f.left->tabText(0), QStringLiteral("a.pdf *"));
        QCOMPARE(f.right->tabText(0), QStringLiteral("a.pdf *"));

        left->document()->undoStack()->undo();
    }

    void search()
    {
        Fixture f;
        f.window.openFile(m_a); // pages are labelled "A 1" … "A 6"
        PdfView *v = f.left->currentView();
        auto *edit = f.window.findChild<QLineEdit *>(QStringLiteral("findEdit"));
        auto *status = f.window.findChild<QLabel *>(QStringLiteral("findStatus"));
        QVERIFY(edit && status);
        QVERIFY(!edit->isVisible());

        QCOMPARE(action(f.window, QStringLiteral("Find…"))->shortcut(), QKeySequence(QKeySequence::Find));
        trigger(f.window, QStringLiteral("Find…"));
        QVERIFY(edit->isVisible());

        // Typing searches the whole document; case is ignored by default.
        QTest::keyClicks(edit, QStringLiteral("a"));
        QTRY_COMPARE(status->text(), QStringLiteral("1 of 6"));
        QCOMPARE(v->matchCount(), 6);
        QCOMPARE(v->currentPage(), 0);

        // Enter goes forward, Shift+Enter back, both wrap around.
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(status->text(), QStringLiteral("2 of 6"));
        QCOMPARE(v->currentPage(), 1);
        QTest::keyClick(edit, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClick(edit, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(status->text(), QStringLiteral("6 of 6"));
        QCOMPARE(v->currentPage(), 5);
        trigger(f.window, QStringLiteral("Find Next"));
        QCOMPARE(status->text(), QStringLiteral("1 of 6"));
        QCOMPARE(v->currentPage(), 0);

        // The current match is painted on the page.
        QTRY_VERIFY(!f.window.isRendering());
        const QColor marked = colorAtText(v, 0);
        QVERIFY2(marked.red() > 240 && marked.blue() < 160, qPrintable(marked.name()));

        // Refining the query jumps to the page that has it.
        edit->setText(QStringLiteral("4"));
        QTRY_COMPARE(status->text(), QStringLiteral("1 of 1"));
        QCOMPARE(v->currentPage(), 3);

        // Nothing found.
        edit->setText(QStringLiteral("zebra"));
        QTRY_COMPARE(status->text(), QStringLiteral("No matches"));
        QCOMPARE(v->matchCount(), 0);

        // Match case.
        edit->setText(QStringLiteral("a"));
        QTRY_COMPARE(v->matchCount(), 6);
        f.window.findChild<QCheckBox *>()->setChecked(true);
        QTRY_COMPARE(status->text(), QStringLiteral("No matches"));
        f.window.findChild<QCheckBox *>()->setChecked(false);
        QTRY_COMPARE(v->matchCount(), 6);

        // Another tab is searched for the same text once it is current.
        f.window.openFile(m_b); // "B 1" … "B 3"
        PdfView *b = f.left->currentView();
        QTRY_COMPARE(status->text(), QStringLiteral("No matches"));
        edit->setText(QStringLiteral("b"));
        QTRY_COMPARE(status->text(), QStringLiteral("1 of 3"));
        QCOMPARE(b->matchCount(), 3);

        // Esc closes the bar and clears the marks.
        QTest::keyClick(edit, Qt::Key_Escape);
        QVERIFY(!edit->isVisible());
        QCOMPARE(b->matchCount(), 0);

        // Ctrl+F with text selected searches for it.
        f.left->setCurrentIndex(0);
        v->goToPage(1);
        dragAcrossText(v, 1);
        QCOMPARE(v->selectedText(), QStringLiteral("A 2"));
        trigger(f.window, QStringLiteral("Find…"));
        QCOMPARE(edit->text(), QStringLiteral("A 2"));
        QTest::keyClick(edit, Qt::Key_Escape);
    }

    void darkMode()
    {
        {
            Fixture f;
            QVERIFY(!f.window.darkMode());
            f.window.openFile(m_a);
            PdfView *v = f.left->currentView();
            QTRY_VERIFY(!f.window.isRendering());
            const QColor lightUi = f.window.palette().color(QPalette::Window);
            QCOMPARE(colorAtText(v, 0), QColor(Qt::white));

            QCOMPARE(action(f.window, QStringLiteral("Dark Mode"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_D));
            trigger(f.window, QStringLiteral("Dark Mode"));
            QVERIFY(f.window.darkMode());
            QVERIFY(action(f.window, QStringLiteral("Dark Mode"))->isChecked());
            QTRY_VERIFY(!f.window.isRendering());

            // Paper is dark grey, the blue band keeps its hue, the UI is dark.
            QCOMPARE(colorAtText(v, 0), PdfView::darkened(Qt::white));
            QVERIFY(colorAtText(v, 0).lightness() < 40);
            v->goToPage(0);
            QTRY_VERIFY(!f.window.isRendering());
            const QImage shot = v->viewport()->grab().toImage();
            const QColor band = shot.pixelColor(shot.width() / 2, 40);
            QVERIFY2(band.blue() > band.red() + 60 && band.blue() > band.green() + 60, qPrintable(band.name()));
            QVERIFY(f.window.palette().color(QPalette::Window).lightness() < 80);

            // A highlight is still yellow-ish on the dark page, before and after the re-render.
            dragAcrossText(v, 0);
            trigger(f.window, QStringLiteral("Highlight Yellow"));
            const auto yellowish = [](const QColor &c) {
                return c.red() > 120 && c.green() > 100 && c.blue() < 60;
            };
            QVERIFY2(yellowish(colorAtText(v, 0)), qPrintable(colorAtText(v, 0).name()));
            QTRY_VERIFY(!f.window.isRendering());
            QVERIFY2(yellowish(colorAtText(v, 0)), qPrintable(colorAtText(v, 0).name()));
            trigger(f.window, QStringLiteral("Undo"));

            // A second tab opens dark as well.
            f.window.openFile(m_b);
            QVERIFY(f.left->currentView()->darkMode());

            Q_UNUSED(lightUi)
        }
        {
            // Remembered across restarts; switching off restores everything.
            Fixture f;
            QVERIFY(f.window.darkMode());
            f.window.openFile(m_a);
            PdfView *v = f.left->currentView();
            QTRY_VERIFY(!f.window.isRendering());
            QVERIFY(colorAtText(v, 0).lightness() < 40);
            trigger(f.window, QStringLiteral("Dark Mode"));
            QTRY_VERIFY(!f.window.isRendering());
            QCOMPARE(colorAtText(v, 0), QColor(Qt::white));
            QVERIFY(f.window.palette().color(QPalette::Window).lightness() > 150);
        }
        Fixture f;
        QVERIFY(!f.window.darkMode());
    }

    void darkModeWithoutDarkPages()
    {
        Fixture f;
        f.window.openFile(m_a);
        PdfView *v = f.left->currentView();
        QAction *pages = action(f.window, QStringLiteral("Dark Pages in Dark Mode"));
        QVERIFY(pages->isChecked()); // default: pages go dark too

        // Option off: dark interface, untouched document.
        pages->trigger();
        trigger(f.window, QStringLiteral("Dark Mode"));
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(f.window.palette().color(QPalette::Window).lightness() < 80);
        QCOMPARE(colorAtText(v, 0), QColor(Qt::white));
        QVERIFY(!v->darkMode());

        // Switching the option while in dark mode applies at once.
        pages->trigger();
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(colorAtText(v, 0).lightness() < 40);
        pages->trigger();
        QTRY_VERIFY(!f.window.isRendering());
        QCOMPARE(colorAtText(v, 0), QColor(Qt::white));

        // The choice is remembered.
        {
            Fixture again;
            QVERIFY(again.window.darkMode());
            QVERIFY(!action(again.window, QStringLiteral("Dark Pages in Dark Mode"))->isChecked());
            again.window.openFile(m_b);
            QVERIFY(!again.left->currentView()->darkMode());
            action(again.window, QStringLiteral("Dark Pages in Dark Mode"))->trigger();
            trigger(again.window, QStringLiteral("Dark Mode"));
        }
    }

    void tabDragBetweenPanes()
    {
        Fixture f;
        f.window.openFiles({m_a, m_b});
        const QString mimeType = QString::fromLatin1(TabPane::tabMimeType());

        // Dragging a tab out of the bar starts a drag that names the tab.
        QByteArray dragged;
        const auto savedExec = TabPane::execDrag;
        TabPane::execDrag = [&](QDrag *drag) {
            dragged = drag->mimeData()->data(mimeType);
            delete drag;
            return Qt::IgnoreAction; // Esc, over the window
        };
        QTabBar *bar = f.left->tabBar();
        const QPoint onTab = bar->tabRect(1).center();
        QTest::mousePress(bar, Qt::LeftButton, {}, onTab);
        // QTest's own move helper sends no button state, which a drag needs.
        const auto move = [bar](const QPoint &pos) {
            QMouseEvent event(QEvent::MouseMove, pos, bar->mapToGlobal(pos), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(bar, &event);
        };
        move(onTab + QPoint(5, 0)); // inside the bar: Qt's reordering, no drag
        QVERIFY(dragged.isEmpty());
        move(onTab + QPoint(40, 200));
        const QJsonObject payload = QJsonDocument::fromJson(dragged).object();
        QCOMPARE(payload.value(QStringLiteral("window")).toString(), f.window.windowId());
        QCOMPARE(payload.value(QStringLiteral("pane")).toInt(), 0);
        QCOMPARE(payload.value(QStringLiteral("index")).toInt(), 1);
        QCOMPARE(payload.value(QStringLiteral("path")).toString(), QFileInfo(m_b).absoluteFilePath());
        QCOMPARE(f.left->count(), 2); // cancelled: nothing happened
        TabPane::execDrag = savedExec;

        const auto drop = [&](const QByteArray &payload, const QPoint &pos) {
            QMimeData mime;
            mime.setData(mimeType, payload);
            return dropOn(&f.window, &mime, pos) == Qt::MoveAction;
        };
        const auto tab = [&](int pane, int index) {
            return tabPayload(f.window, pane, index);
        };
        const auto inWindow = [&](QWidget *w, const QPoint &p) {
            return w->mapTo(&f.window, p);
        };

        // Dropping on the tab's own half does nothing.
        QVERIFY(!drop(tab(0, 1), inWindow(f.left, QPoint(50, 300))));
        QCOMPARE(f.left->count(), 2);

        // Dropping on the right half of an unsplit window creates the split.
        QVERIFY(drop(tab(0, 1), inWindow(f.left, QPoint(f.left->width() - 50, 300))));
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.right->count(), 1);
        QVERIFY(f.right->isVisible());
        QCOMPARE(f.right->currentView()->document()->fileName(), QStringLiteral("b.pdf"));

        // Dropping onto a tab in the other pane takes that tab's place.
        f.window.openFile(m_a, 1); // right: b, a
        QCOMPARE(f.right->count(), 2);
        QVERIFY(drop(tab(1, 1), inWindow(f.left->tabBar(), f.left->tabBar()->tabRect(0).center())));
        QCOMPARE(f.left->count(), 2);
        QCOMPARE(f.right->count(), 1);
        QCOMPARE(f.left->currentIndex(), 0); // inserted in front, and shown

        // Dropping below the tabs appends.
        QVERIFY(drop(tab(1, 0), inWindow(f.left, QPoint(60, 300))));
        QCOMPARE(f.left->count(), 3);
        QCOMPARE(f.left->currentIndex(), 2);
        QVERIFY(!f.right->isVisible());

        // The only tab of an unsplit window has nowhere to go; junk is ignored.
        f.window.unsavedPrompt = [](const QString &) { return QMessageBox::Discard; };
        trigger(f.window, QStringLiteral("Close Tab"));
        trigger(f.window, QStringLiteral("Close Tab"));
        QVERIFY(!drop(tab(0, 0), inWindow(f.left, QPoint(f.left->width() - 50, 300))));
        QVERIFY(!drop(tab(0, 7), inWindow(f.left, QPoint(f.left->width() - 50, 300))));
        QVERIFY(!drop("nonsense", inWindow(f.left, QPoint(f.left->width() - 50, 300))));
        QCOMPARE(f.left->count(), 1);
    }

    // A tab dropped on another window (in this test the same process, but
    // the payload is all a window in another process would get) opens there
    // at the same place and leaves the window it came from.
    void tabDragBetweenWindows()
    {
        Fixture f;
        Fixture g;
        f.window.unsavedPrompt = [](const QString &) { return QMessageBox::Discard; };
        f.window.openFiles({m_a, m_b});
        f.left->setCurrentIndex(0);
        PdfView *source = f.left->currentView();
        source->setZoom(1.5);
        source->goToPage(3);
        g.window.openFile(m_b);

        const auto savedExec = TabPane::execDrag;
        const auto restore = qScopeGuard([&] { TabPane::execDrag = savedExec; });
        QPoint target;
        TabPane::execDrag = [&](QDrag *drag) {
            const Qt::DropAction action = dropOn(&g.window, drag->mimeData(), target);
            delete drag;
            return action;
        };

        // Onto the tab bar of the other window: inserted before b.pdf.
        target = g.left->tabBar()->mapTo(&g.window, g.left->tabBar()->tabRect(0).center());
        dragTabOut(f.left, 0);
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.left->tabText(0), QStringLiteral("b.pdf"));
        QCOMPARE(g.left->count(), 2);
        QCOMPARE(g.left->tabText(0), QStringLiteral("a.pdf"));
        PdfView *moved = g.left->currentView();
        QCOMPARE(moved->document()->fileName(), QStringLiteral("a.pdf"));
        QCOMPARE(moved->zoomMode(), PdfView::ZoomMode::Custom);
        QCOMPARE(moved->zoom(), 1.5);
        QCOMPARE(moved->currentPage(), 3);

        // Unsaved annotations would not survive the trip: refused, with a hint.
        f.window.openFile(m_a);
        f.left->currentView()->document()->addAnnotation(
            {QString(), 0, AnnotationData::Kind::Note, Qt::yellow, QRectF(0.1, 0.1, 0.05, 0.05), {}, QStringLiteral("hi"), 12, {}, 1});
        dragTabOut(f.left, f.left->currentIndex());
        QCOMPARE(f.left->count(), 2);
        QCOMPARE(g.left->count(), 2);
        QVERIFY(g.window.statusBar()->currentMessage().contains(QStringLiteral("a.pdf")));

        // The only tab of a window can go too; an empty window stays behind.
        trigger(f.window, QStringLiteral("Close Tab"));
        target = g.left->mapTo(&g.window, QPoint(g.left->width() - 50, 300)); // right half: split there
        dragTabOut(f.left, 0);
        QCOMPARE(f.left->count(), 0);
        QCOMPARE(g.right->count(), 1);
        QCOMPARE(g.right->tabText(0), QStringLiteral("b.pdf"));
    }

    // Dropped outside every window, a tab moves into a new yap-dfe process.
    void tabDetach()
    {
        Fixture f;
        f.window.openFiles({m_a, m_b});
        f.left->setCurrentIndex(0);
        f.left->currentView()->goToPage(2);

        const auto savedExec = TabPane::execDrag;
        const auto savedStart = MainWindow::startInstance;
        const auto restore = qScopeGuard([&] {
            TabPane::execDrag = savedExec;
            MainWindow::startInstance = savedStart;
        });
        QStringList started;
        MainWindow::startInstance = [&](const QStringList &arguments) {
            started = arguments;
            return true;
        };
        bool leave = true;
        TabPane::execDrag = [&](QDrag *drag) {
            // Over the window, then (maybe) out of it, then let go.
            QDragEnterEvent enter(QPointF(100, 100), Qt::MoveAction, drag->mimeData(), Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&f.window, &enter);
            if (leave) {
                QDragLeaveEvent leaveEvent;
                QApplication::sendEvent(&f.window, &leaveEvent);
            }
            delete drag;
            return Qt::IgnoreAction;
        };

        // Let go over the window: nothing happens.
        leave = false;
        dragTabOut(f.left, 0);
        QVERIFY(started.isEmpty());
        QCOMPARE(f.left->count(), 2);

        leave = true;
        dragTabOut(f.left, 0);
        QCOMPARE(f.left->count(), 1);
        QCOMPARE(f.left->tabText(0), QStringLiteral("b.pdf"));
        QVERIFY(started.contains(QStringLiteral("--new-window")));
        QCOMPARE(started.constLast(), QFileInfo(m_a).absoluteFilePath());
        const int stateAt = started.indexOf(QStringLiteral("--view-state"));
        QVERIFY(stateAt >= 0);
        const PdfView::ViewState state = MainWindow::viewStateFromJson(QJsonDocument::fromJson(started.at(stateAt + 1).toUtf8()).object());
        QCOMPARE(state.page, 2);

        // The new process reads the file, so unsaved annotations are saved
        // first, or the tab stays.
        const QString copy = m_dir.filePath(QStringLiteral("detach.pdf"));
        QVERIFY(QFile::copy(m_a, copy));
        f.window.openFile(copy);
        const auto doc = f.left->currentView()->document();
        doc->addAnnotation({QString(), 0, AnnotationData::Kind::Note, Qt::yellow, QRectF(0.1, 0.1, 0.05, 0.05), {}, QStringLiteral("hi"), 12, {}, 1});
        started.clear();
        f.window.saveBeforeDetach = [](const std::shared_ptr<Document> &) { return false; };
        dragTabOut(f.left, f.left->currentIndex());
        QVERIFY(started.isEmpty());
        QCOMPARE(f.left->count(), 2);
        QString error;
        QVERIFY(doc->save(doc->path(), &error)); // what the real prompt's Save does
        f.window.saveBeforeDetach = [](const std::shared_ptr<Document> &) { return true; };
        dragTabOut(f.left, f.left->currentIndex());
        QVERIFY(!started.isEmpty());
        QCOMPARE(f.left->count(), 1);

        // The only tab has nowhere to go: the window already is its window.
        started.clear();
        dragTabOut(f.left, 0);
        QVERIFY(started.isEmpty());
        QCOMPARE(f.left->count(), 1);

        // The view state option the new process gets round-trips.
        PdfView::ViewState in;
        in.mode = PdfView::ZoomMode::FitPage;
        in.zoom = 0.75;
        in.page = 4;
        in.fx = 0.25;
        in.fy = 0.5;
        const PdfView::ViewState out = MainWindow::viewStateFromJson(MainWindow::viewStateToJson(in));
        QCOMPARE(out.mode, in.mode);
        QCOMPARE(out.zoom, in.zoom);
        QCOMPARE(out.page, in.page);
        QCOMPARE(out.fx, in.fx);
        QCOMPARE(out.fy, in.fy);
    }

    void pen()
    {
        const QString copy = m_dir.filePath(QStringLiteral("pen.pdf"));
        QVERIFY(QFile::copy(m_b, copy));
        Fixture f;
        f.window.openFile(copy);
        PdfView *v = f.left->currentView();
        const std::shared_ptr<Document> doc = v->document();
        QTRY_VERIFY(!f.window.isRendering());
        const auto isRed = [](const QColor &c) {
            return c.red() > 180 && c.green() < 110 && c.blue() < 110;
        };
        const auto colorAt = [v](QPointF n) {
            return v->viewport()->grab().toImage().pixelColor(v->viewportPoint(0, n));
        };

        QCOMPARE(action(f.window, QStringLiteral("Pen"))->shortcut(), QKeySequence(Qt::ALT | Qt::Key_0));
        trigger(f.window, QStringLiteral("Pen"));
        QCOMPARE(f.window.tool(), PdfView::Tool::Pen);

        // Draw a line across white paper, below the blue band.
        const QPointF a(0.2, 0.3), b(0.4, 0.3), mid(0.3, 0.3);
        const auto stroke = [&](QPointF from, QPointF to) {
            QTest::mousePress(v->viewport(), Qt::LeftButton, {}, v->viewportPoint(0, from));
            QTest::mouseMove(v->viewport(), v->viewportPoint(0, (from + to) / 2));
            QVERIFY(isRed(colorAt((from * 3 + to) / 4))); // visible while drawing
            QTest::mouseMove(v->viewport(), v->viewportPoint(0, to));
            QTest::mouseRelease(v->viewport(), Qt::LeftButton, {}, v->viewportPoint(0, to));
        };
        QCOMPARE(colorAt(mid), QColor(Qt::white));
        stroke(a, b);
        QCOMPARE(doc->annotations(0).size(), 1);
        const AnnotationData ink = doc->annotations(0).first();
        QCOMPARE(ink.kind, AnnotationData::Kind::Ink);
        QVERIFY(isRed(ink.color));
        QCOMPARE(ink.paths.size(), 1);
        QVERIFY(qAbs(ink.paths.first().first().x() - a.x()) < 0.01);
        QVERIFY(qAbs(ink.paths.first().last().x() - b.x()) < 0.01);
        QVERIFY(doc->isModified());
        QVERIFY(isRed(colorAt(mid))); // shown immediately...
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(isRed(colorAt(mid))); // ...and in the re-rendered page

        // The tool stays on; every stroke is undone on its own.
        QCOMPARE(f.window.tool(), PdfView::Tool::Pen);
        stroke(QPointF(0.2, 0.36), QPointF(0.4, 0.38));
        QCOMPARE(doc->annotations(0).size(), 2);
        trigger(f.window, QStringLiteral("Undo"));
        QCOMPARE(doc->annotations(0).size(), 1);
        QTRY_VERIFY(!f.window.isRendering());
        QCOMPARE(colorAt(QPointF(0.3, 0.37)), QColor(Qt::white));

        // Saved as a standard ink annotation, and read back.
        trigger(f.window, QStringLiteral("Save"));
        Document::LoadStatus status;
        const auto reloaded = Document::load(copy, QString(), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        QCOMPARE(reloaded->annotations(0).size(), 1);
        const AnnotationData back = reloaded->annotations(0).first();
        QCOMPARE(back.kind, AnnotationData::Kind::Ink);
        QVERIFY(isRed(back.color));
        QCOMPARE(back.paths.size(), 1);
        QVERIFY(qAbs(back.paths.first().first().x() - a.x()) < 0.01);
        QVERIFY(qAbs(back.paths.first().first().y() - a.y()) < 0.01);
        QVERIFY(qAbs(back.width - ink.width) < 0.01);

        // With Select: a click on the line selects it, next to it does not.
        QTest::keyClick(v, Qt::Key_Escape);
        QCOMPARE(f.window.tool(), PdfView::Tool::Select);
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->viewportPoint(0, QPointF(0.3, 0.36)));
        QVERIFY(v->selectedAnnotation().isEmpty());
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->viewportPoint(0, mid));
        QCOMPARE(v->selectedAnnotation(), ink.id);

        // Dragging moves the whole stroke.
        const QPoint from = v->viewportPoint(0, mid), to = v->viewportPoint(0, QPointF(0.3, 0.38));
        QTest::mousePress(v->viewport(), Qt::LeftButton, {}, from);
        QTest::mouseMove(v->viewport(), (from + to) / 2);
        QTest::mouseMove(v->viewport(), to);
        QTest::mouseRelease(v->viewport(), Qt::LeftButton, {}, to);
        QVERIFY(qAbs(doc->annotations(0).first().paths.first().first().y() - 0.38) < 0.01);
        QTRY_VERIFY(!f.window.isRendering());
        QVERIFY(isRed(colorAt(QPointF(0.3, 0.38))));
        QCOMPARE(colorAt(mid), QColor(Qt::white));

        trigger(f.window, QStringLiteral("Delete Annotations"));
        QCOMPARE(doc->annotations(0).size(), 0);
        f.window.unsavedPrompt = [](const QString &) { return QMessageBox::Discard; };
    }

    void multiSelect()
    {
        const QString copy = m_dir.filePath(QStringLiteral("multi.pdf"));
        QVERIFY(QFile::copy(m_b, copy));
        Fixture f;
        f.window.openFile(copy);
        PdfView *v = f.left->currentView();
        const std::shared_ptr<Document> doc = v->document();
        QTRY_VERIFY(!f.window.isRendering());

        // Three notes on white paper below the blue band.
        const auto addNote = [&](QPointF at) {
            AnnotationData d;
            d.kind = AnnotationData::Kind::Note;
            d.color = PdfView::noteColor();
            d.contents = QStringLiteral("note");
            d.boundary = QRectF(at, QSizeF(0.03, 0.02));
            return doc->addAnnotation(d);
        };
        const QString a = addNote(QPointF(0.2, 0.3)), b = addNote(QPointF(0.4, 0.3)), c = addNote(QPointF(0.7, 0.3));
        QTRY_VERIFY(!f.window.isRendering());
        const auto ids = [v] {
            QStringList list;
            for (const AnnotationData &d : v->selectedAnnotations()) {
                list.append(d.id);
            }
            list.sort();
            return list;
        };
        const auto sorted = [](QStringList list) {
            list.sort();
            return list;
        };
        const auto drag = [v](QPointF from, QPointF to, Qt::KeyboardModifiers mods = {}) {
            const QPoint p = v->viewportPoint(0, from), q = v->viewportPoint(0, to);
            QTest::mousePress(v->viewport(), Qt::LeftButton, mods, p);
            QTest::mouseMove(v->viewport(), (p + q) / 2);
            QTest::mouseMove(v->viewport(), q);
            QTest::mouseRelease(v->viewport(), Qt::LeftButton, mods, q);
        };
        QAction *del = action(f.window, QStringLiteral("Delete Annotations"));
        QVERIFY(!del->isEnabled());

        // A rectangle dragged from blank paper selects what it touches.
        drag(QPointF(0.1, 0.27), QPointF(0.5, 0.34));
        QCOMPARE(ids(), sorted({a, b}));
        QVERIFY(del->isEnabled());

        // Ctrl+click adds and removes single annotations.
        const QPoint onC = v->viewportPoint(0, QPointF(0.715, 0.31));
        QTest::mouseClick(v->viewport(), Qt::LeftButton, Qt::ControlModifier, onC);
        QCOMPARE(ids(), sorted({a, b, c}));
        QTest::mouseClick(v->viewport(), Qt::LeftButton, Qt::ControlModifier, onC);
        QCOMPARE(ids(), sorted({a, b}));

        // Shift+drag adds to the selection.
        drag(QPointF(0.65, 0.27), QPointF(0.8, 0.34), Qt::ShiftModifier);
        QCOMPARE(ids(), sorted({a, b, c}));

        // A plain click on blank paper clears; Ctrl+A selects everything.
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->viewportPoint(0, QPointF(0.5, 0.4)));
        QVERIFY(ids().isEmpty());
        trigger(f.window, QStringLiteral("Select All Annotations"));
        QCOMPARE(ids(), sorted({a, b, c}));
        QTest::keyClick(v, Qt::Key_Escape);
        QVERIFY(ids().isEmpty());

        // Dragging one of the selected moves them all, undone in one step.
        drag(QPointF(0.1, 0.27), QPointF(0.5, 0.34));
        const int undoBefore = doc->undoStack()->count();
        drag(QPointF(0.215, 0.31), QPointF(0.215, 0.41));
        QCOMPARE(doc->undoStack()->count(), undoBefore + 1);
        const auto boundaryOf = [doc](const QString &id) {
            for (const AnnotationData &d : doc->annotations(0)) {
                if (d.id == id) {
                    return d.boundary;
                }
            }
            return QRectF();
        };
        QVERIFY(qAbs(boundaryOf(a).y() - 0.4) < 0.01);
        QVERIFY(qAbs(boundaryOf(b).y() - 0.4) < 0.01);
        QVERIFY(qAbs(boundaryOf(c).y() - 0.3) < 0.01);
        QCOMPARE(ids(), sorted({a, b}));

        // Delete removes all of them; one Undo brings them all back.
        trigger(f.window, QStringLiteral("Delete Annotations"));
        QCOMPARE(doc->annotations(0).size(), 1);
        QVERIFY(ids().isEmpty());
        QVERIFY(!del->isEnabled());
        trigger(f.window, QStringLiteral("Undo"));
        QCOMPARE(doc->annotations(0).size(), 3);
        trigger(f.window, QStringLiteral("Undo"));
        QVERIFY(qAbs(boundaryOf(a).y() - 0.3) < 0.01);
        QVERIFY(qAbs(boundaryOf(b).y() - 0.3) < 0.01);

        f.window.unsavedPrompt = [](const QString &) { return QMessageBox::Discard; };
    }

    void pageRanges()
    {
        QString error;
        const auto parse = [&error](const QString &text, int n) {
            return Document::parsePageRanges(text, n, &error);
        };
        QCOMPARE(parse(QStringLiteral("1-3, 5, 8-"), 10), (QList<int>{0, 1, 2, 4, 7, 8, 9}));
        QCOMPARE(parse(QStringLiteral("3 - 5"), 10), (QList<int>{2, 3, 4}));
        QCOMPARE(parse(QStringLiteral("3–5"), 10), (QList<int>{2, 3, 4})); // en dash
        QCOMPARE(parse(QStringLiteral("-2 2"), 10), (QList<int>{0, 1}));
        QCOMPARE(parse(QStringLiteral("5-3"), 10), (QList<int>{2, 3, 4}));
        QVERIFY(!parse(QStringLiteral("0"), 10));
        QVERIFY(!parse(QStringLiteral("11"), 10));
        QVERIFY(error.contains(QStringLiteral("11")));
        QVERIFY(!parse(QStringLiteral("a"), 10));
        QVERIFY(!parse(QStringLiteral("-"), 10));
        QVERIFY(!parse(QStringLiteral(" "), 10));
        QCOMPARE(Document::formatPageRanges({0, 1, 2, 4, 7, 8}), QStringLiteral("1-3, 5, 8-9"));
    }

    void removeAndExtractPages()
    {
        const QString copy = m_dir.filePath(QStringLiteral("pages.pdf"));
        QVERIFY(QFile::copy(m_a, copy)); // six pages, "A 1" … "A 6"
        Fixture f;
        f.window.openFile(copy);
        f.window.unsavedPrompt = [](const QString &) { return QMessageBox::Discard; };
        PdfView *v = f.left->currentView();
        const std::shared_ptr<Document> doc = v->document();
        QTRY_VERIFY(!f.window.isRendering());
        const auto label = [](const std::shared_ptr<Document> &d, int page) {
            QString s;
            for (const TextChar &c : *d->pageText(page)) {
                s += c.text;
            }
            return s.remove(QLatin1Char(' '));
        };

        // An unsaved note on page 4 goes along with its page.
        AnnotationData note;
        note.page = 3;
        note.kind = AnnotationData::Kind::Note;
        note.color = PdfView::noteColor();
        note.contents = QStringLiteral("on four");
        note.boundary = QRectF(0.2, 0.3, 0.03, 0.02);
        doc->addAnnotation(note);

        // A bad answer asks again, saying what was wrong.
        QStringList answers{QStringLiteral("9"), QStringLiteral("2, 5-")};
        QStringList labels;
        f.window.pageRangePrompt = [&](const QString &, const QString &text, const QString &) {
            labels.append(text);
            return answers.takeFirst();
        };
        trigger(f.window, QStringLiteral("Remove Pages…"));
        QCOMPARE(labels.size(), 2);
        QVERIFY(labels.last().contains(QStringLiteral("9")));
        QCOMPARE(doc->pageCount(), 3);
        QCOMPARE(v->pageCount(), 3);
        QCOMPARE(label(doc, 0), QStringLiteral("A1"));
        QCOMPARE(label(doc, 1), QStringLiteral("A3"));
        QCOMPARE(label(doc, 2), QStringLiteral("A4"));
        QCOMPARE(doc->annotations(2).size(), 1);
        QCOMPARE(doc->annotations(2).first().contents, QStringLiteral("on four"));
        QVERIFY(doc->isModified());
        QTRY_VERIFY(!f.window.isRendering());

        // One Undo brings the pages back, and Redo removes them again.
        trigger(f.window, QStringLiteral("Undo"));
        QCOMPARE(doc->pageCount(), 6);
        QCOMPARE(label(doc, 1), QStringLiteral("A2"));
        QCOMPARE(doc->annotations(3).size(), 1);
        trigger(f.window, QStringLiteral("Redo"));
        QCOMPARE(doc->pageCount(), 3);
        QTRY_VERIFY(!f.window.isRendering());

        // Annotations still work on the new pages.
        AnnotationData later = note;
        later.page = 0;
        later.contents = QStringLiteral("after removal");
        doc->addAnnotation(later);
        QCOMPARE(doc->annotations(0).size(), 1);

        // Extract pages 1 and 3 (= "A 1" and "A 4") into a new file.
        const QString extracted = m_dir.filePath(QStringLiteral("extracted.pdf"));
        f.window.pageRangePrompt = [](const QString &, const QString &, const QString &) { return QStringLiteral("1, 3"); };
        QString suggested;
        f.window.extractPathPrompt = [&](const QString &s) {
            suggested = s;
            return extracted;
        };
        trigger(f.window, QStringLiteral("Extract Pages…"));
        QCOMPARE(QFileInfo(suggested).fileName(), QStringLiteral("pages_pages_1_3.pdf"));
        Document::LoadStatus status;
        const auto out = Document::load(extracted, QString(), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        QCOMPARE(out->pageCount(), 2);
        QCOMPARE(label(out, 0), QStringLiteral("A1"));
        QCOMPARE(label(out, 1), QStringLiteral("A4"));
        QCOMPARE(out->annotations(0).size(), 1);
        QCOMPARE(out->annotations(1).size(), 1);
        QCOMPARE(doc->pageCount(), 3); // the document itself is unchanged

        // Saving writes the shortened document.
        trigger(f.window, QStringLiteral("Save"));
        QVERIFY(!doc->isModified());
        const auto saved = Document::load(copy, QString(), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        QCOMPARE(saved->pageCount(), 3);
        QCOMPARE(label(saved, 2), QStringLiteral("A4"));
        QCOMPARE(saved->annotations(2).size(), 1);

        // Not every page can go.
        QString error;
        QVERIFY(!doc->removePages({0, 1, 2}, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(doc->pageCount(), 3);
    }

    void links()
    {
        Document::LoadStatus status;
        const auto doc = Document::load(m_links, QString(), &status);
        QCOMPARE(status, Document::LoadStatus::Ok);
        const QList<LinkData> list = doc->links(0);
        QCOMPARE(list.size(), 4);
        QCOMPARE(list.at(0).kind, LinkData::Kind::Goto);
        QCOMPARE(list.at(0).page, 2);
        QVERIFY(qAbs(list.at(0).top - 0.5) < 0.01);
        QCOMPARE(list.at(1).kind, LinkData::Kind::Goto); // named destination, resolved
        QCOMPARE(list.at(1).page, 1);
        QCOMPARE(list.at(2).kind, LinkData::Kind::Uri);
        QCOMPARE(list.at(2).target, QStringLiteral("https://example.org/"));
        QCOMPARE(list.at(3).kind, LinkData::Kind::Action);
        QCOMPARE(list.at(3).action, LinkData::Action::LastPage);
        QVERIFY(doc->links(1).isEmpty());

        Fixture f;
        f.window.openFile(m_links);
        PdfView *v = f.left->currentView();
        QTRY_VERIFY(!f.window.isRendering());
        QUrl opened;
        const auto savedOpener = PdfView::urlOpener;
        const auto restore = qScopeGuard([&] { PdfView::urlOpener = savedOpener; });
        PdfView::urlOpener = [&](QWidget *, const QUrl &url) { opened = url; };

        const auto top = [v] { return v->viewState(); };
        const auto click = [&](int link) {
            v->goToPage(0);
            const QPoint p = v->viewportPoint(0, list.at(link).area.center());
            QVERIFY(v->viewport()->rect().contains(p));
            QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
        };
        QVERIFY(!action(f.window, QStringLiteral("Back"))->isEnabled());

        // The chapter three entry: half way down page 3, at the top of the view.
        click(0);
        QCOMPARE(top().page, 2);
        QVERIFY(qAbs(top().fy - 0.5) < 0.05);
        QVERIFY(action(f.window, QStringLiteral("Back"))->isEnabled());
        trigger(f.window, QStringLiteral("Back"));
        QCOMPARE(v->currentPage(), 0);
        trigger(f.window, QStringLiteral("Forward"));
        QCOMPARE(top().page, 2);
        QVERIFY(!action(f.window, QStringLiteral("Forward"))->isEnabled());

        click(1); // named destination: top of page 2
        QCOMPARE(v->currentPage(), 1);
        click(2);
        QCOMPARE(opened, QUrl(QStringLiteral("https://example.org/")));
        QCOMPARE(v->currentPage(), 0);
        click(3);
        QCOMPARE(v->currentPage(), 3);

        // The mouse's back button goes back too.
        QTest::mouseClick(v->viewport(), Qt::BackButton, {}, QPoint(50, 50));
        QCOMPARE(v->currentPage(), 0);

        // Dragging across a link selects its text instead of following it.
        v->goToPage(0);
        const auto text = doc->pageText(0);
        QVERIFY(list.at(0).area.contains(text->first().rect.center()));
        const QPoint from = v->viewportPoint(0, text->first().rect.center());
        const QPoint to = v->viewportPoint(0, text->at(5).rect.center());
        QTest::mousePress(v->viewport(), Qt::LeftButton, {}, from);
        QTest::mouseMove(v->viewport(), (from + to) / 2);
        QTest::mouseMove(v->viewport(), to);
        QTest::mouseRelease(v->viewport(), Qt::LeftButton, {}, to);
        QCOMPARE(v->currentPage(), 0);
        QVERIFY(v->hasTextSelection());
        QVERIFY(v->selectedText().startsWith(QStringLiteral("Chap")));

        // With a marker tool a click on a link does not follow it.
        v->clearTextSelection();
        trigger(f.window, QStringLiteral("Highlight Yellow"));
        QCOMPARE(v->tool(), PdfView::Tool::Highlight);
        opened.clear();
        click(2);
        QVERIFY(opened.isEmpty());
        trigger(f.window, QStringLiteral("Select"));

        // Go to Page can be undone with Back as well.
        v->goToPage(0);
        v->jumpTo(3);
        trigger(f.window, QStringLiteral("Back"));
        QCOMPARE(v->currentPage(), 0);
    }

    // The status bar describes the view being scrolled or zoomed, also when
    // that is the other pane.
    void statusFollowsScrolledPane()
    {
        Fixture f;
        f.window.openFile(m_a);
        trigger(f.window, QStringLiteral("Split View"));
        PdfView *left = f.left->currentView();
        PdfView *right = f.right->currentView();
        right->setFocus();
        QTRY_VERIFY(right->hasFocus());
        const auto labels = f.window.statusBar()->findChildren<QLabel *>();
        const auto zoomText = [&] {
            for (const QLabel *l : labels) {
                if (l->text().contains(QLatin1Char('%'))) {
                    return l->text();
                }
            }
            return QString();
        };
        QVERIFY(zoomText().contains(QStringLiteral("fit width")));

        const QPoint p(100, 100);
        QWheelEvent wheel(p, left->viewport()->mapToGlobal(p), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(left->viewport(), &wheel);
        QVERIFY(left->hasFocus());
        QCOMPARE(left->zoomMode(), PdfView::ZoomMode::Custom);
        QCOMPARE(zoomText(), QStringLiteral("%1 %").arg(qRound(left->zoom() * 100)));
        QVERIFY(qRound(left->zoom() * 100) != qRound(right->zoom() * 100));
    }

    void sessionRestore()
    {
        const QString gone = m_dir.filePath(QStringLiteral("gone.pdf"));
        QVERIFY(QFile::copy(m_b, gone));
        {
            Fixture f;
            f.window.restoreSession(); // nothing saved yet
            QCOMPARE(f.left->count(), 0);

            f.window.openFiles({m_a, m_b, gone});
            f.left->setCurrentIndex(0);
            f.left->currentView()->goToPage(4);
            trigger(f.window, QStringLiteral("Split View")); // a.pdf on the right too
            PdfView *right = f.right->currentView();
            right->setZoom(2.0);
            right->goToPage(2);
            f.left->setCurrentIndex(1);
            QVERIFY(f.window.close());
        }
        QVERIFY(QFile::remove(gone));
        {
            Fixture f;
            f.window.restoreSession();
            // Same tabs in the same panes; the deleted file is left out.
            QCOMPARE(f.left->count(), 2);
            QCOMPARE(f.right->count(), 1);
            QVERIFY(f.right->isVisible());
            QCOMPARE(f.left->tabText(0), QStringLiteral("a.pdf"));
            QCOMPARE(f.left->tabText(1), QStringLiteral("b.pdf"));
            QCOMPARE(f.left->currentIndex(), 1);
            // Positions and zoom come back.
            QCOMPARE(f.left->view(0)->currentPage(), 4);
            QCOMPARE(f.left->view(0)->zoomMode(), PdfView::ZoomMode::FitWidth);
            const PdfView *right = f.right->currentView();
            QCOMPARE(right->document()->fileName(), QStringLiteral("a.pdf"));
            QCOMPARE(right->zoomMode(), PdfView::ZoomMode::Custom);
            QCOMPARE(right->zoom(), 2.0);
            QCOMPARE(right->currentPage(), 2);
            QCOMPARE(right->document(), f.left->view(0)->document()); // one shared document
            QTRY_VERIFY(!f.window.isRendering());
            QCOMPARE(f.left->view(0)->currentPage(), 4); // still there once laid out and shown

            // Switched off: the next start is empty.
            trigger(f.window, QStringLiteral("Restore Tabs on Startup"));
            QVERIFY(f.window.close());
        }
        {
            Fixture f;
            f.window.restoreSession();
            QCOMPARE(f.left->count(), 0);
            trigger(f.window, QStringLiteral("Restore Tabs on Startup"));
        }
        // An empty window closing leaves an empty session.
        {
            Fixture f;
            QVERIFY(f.window.close());
        }
        Fixture f;
        f.window.restoreSession();
        QCOMPARE(f.left->count(), 0);
    }

    // Last: it leaves a saved session behind.
    void recentFiles()
    {
        // The texts of the Open Recent entries, as the menu would show them now.
        const auto entries = [](MainWindow &w) {
            auto *menu = w.findChild<QMenu *>(QStringLiteral("recentMenu"));
            Q_EMIT menu->aboutToShow();
            QStringList names;
            for (const QAction *a : menu->actions()) {
                if (a->isSeparator()) {
                    break;
                }
                names << a->text().section(QStringLiteral("  —  "), 0, 0);
            }
            return names;
        };
        const QString gone = m_dir.filePath(QStringLiteral("recent-gone.pdf"));
        QVERIFY(QFile::copy(m_b, gone));
        {
            Fixture f;
            auto *menu = f.window.findChild<QMenu *>(QStringLiteral("recentMenu"));
            Q_EMIT menu->aboutToShow();
            menu->actions().constLast()->trigger(); // Clear List: earlier tests opened files too
            QVERIFY(!menu->isEnabled());
            auto *page = f.window.findChild<StartPage *>();
            auto *list = page->findChild<QListWidget *>();
            QVERIFY(page->isVisible());
            QVERIFY(!list->isVisible());

            // Most recently opened first; opening again moves a file to the top.
            f.window.openFiles({m_a, gone, m_b});
            QVERIFY(!page->isVisible());
            QCOMPARE(entries(f.window), QStringList({QStringLiteral("b.pdf"), QStringLiteral("recent-gone.pdf"), QStringLiteral("a.pdf")}));
            trigger(f.window, QStringLiteral("Close Tab"));
            f.window.openFile(m_b);
            QCOMPARE(entries(f.window).size(), 3);
            QVERIFY(f.window.close());
        }
        QVERIFY(QFile::remove(gone));
        {
            // Remembered by the next window; the session's tabs coming back
            // does not reshuffle the list, and the deleted file is left out.
            Fixture f;
            f.window.restoreSession();
            QCOMPARE(f.left->count(), 2);
            QCOMPARE(entries(f.window), QStringList({QStringLiteral("b.pdf"), QStringLiteral("a.pdf")}));

            // With every tab closed the start page offers them.
            trigger(f.window, QStringLiteral("Close Tab"));
            trigger(f.window, QStringLiteral("Close Tab"));
            auto *page = f.window.findChild<StartPage *>();
            auto *list = page->findChild<QListWidget *>();
            QVERIFY(page->isVisible());
            QCOMPARE(page->geometry(), f.left->rect());
            QCOMPARE(list->count(), 2);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(1)).center());
            QCOMPARE(f.left->count(), 1);
            QCOMPARE(f.left->currentView()->document()->fileName(), QStringLiteral("a.pdf"));
            QVERIFY(!page->isVisible());

            // And from the menu.
            auto *menu = f.window.findChild<QMenu *>(QStringLiteral("recentMenu"));
            QCOMPARE(entries(f.window).first(), QStringLiteral("a.pdf"));
            menu->actions().at(1)->trigger();
            QCOMPARE(f.left->count(), 2);
            QCOMPARE(f.left->currentView()->document()->fileName(), QStringLiteral("b.pdf"));
        }
    }
};

QTEST_MAIN(SmokeTest)
#include "smoketest.moc"
