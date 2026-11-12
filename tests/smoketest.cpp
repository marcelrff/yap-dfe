// Drives the real main window offscreen: tabs, split view, zoom, navigation
// and rendering, against PDFs generated on the fly.

#include "document.h"
#include "mainwindow.h"
#include "pdfview.h"
#include "tabpane.h"

#include <QAction>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QClipboard>
#include <QPlainTextEdit>
#include <QPainter>
#include <QDir>
#include <QMessageBox>
#include <QPdfWriter>
#include <QProcess>
#include <QScrollBar>
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
        trigger(f.window, QStringLiteral("Delete Annotation"));
        QCOMPARE(doc->annotations(0).size(), 1);
        QVERIFY(!action(f.window, QStringLiteral("Delete Annotation"))->isEnabled());

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
};

QTEST_MAIN(SmokeTest)
#include "smoketest.moc"
