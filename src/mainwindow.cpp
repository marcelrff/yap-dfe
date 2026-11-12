#include "mainwindow.h"

#include "document.h"
#include "findbar.h"
#include "pdfview.h"
#include "tabpane.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStyleHints>
#include <QToolBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {
QSettings appSettings()
{
    return QSettings(QStringLiteral("yap-dfv"), QStringLiteral("yap-dfv"));
}

QPalette darkPalette()
{
    const QColor text(222, 222, 222);
    const QColor disabled(120, 120, 120);
    QPalette p;
    p.setColor(QPalette::Window, QColor(45, 45, 48));
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, QColor(30, 30, 32));
    p.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
    p.setColor(QPalette::ToolTipBase, QColor(60, 60, 64));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, QColor(140, 140, 140));
    p.setColor(QPalette::Button, QColor(56, 56, 60));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, QColor(90, 170, 255));
    p.setColor(QPalette::Highlight, QColor(61, 174, 233));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, QColor(75, 75, 80));
    p.setColor(QPalette::Midlight, QColor(62, 62, 66));
    p.setColor(QPalette::Mid, QColor(18, 18, 20)); // around the pages
    p.setColor(QPalette::Dark, QColor(24, 24, 26));
    p.setColor(QPalette::Shadow, Qt::black);
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabled);
    }
    return p;
}

// Toolbar icons are drawn here so they look the same on every desktop and
// show the actual marker colours.
QIcon toolIcon(PdfView::Tool tool, const QColor &color, const QColor &ink)
{
    const int size = 64;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QFont font = p.font();
    font.setPixelSize(44);
    font.setBold(true);
    p.setFont(font);
    const QRect box(0, 0, size, size);

    switch (tool) {
    case PdfView::Tool::Select: {
        // An arrow pointer.
        QPainterPath path;
        path.moveTo(16, 8);
        path.lineTo(16, 52);
        path.lineTo(27, 41);
        path.lineTo(35, 58);
        path.lineTo(42, 55);
        path.lineTo(34, 38);
        path.lineTo(50, 38);
        path.closeSubpath();
        p.setPen(QPen(ink, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        break;
    }
    case PdfView::Tool::Highlight:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(box.adjusted(4, 10, -4, -10), 8, 8);
        p.setPen(QColor(30, 30, 30));
        p.drawText(box, Qt::AlignCenter, QStringLiteral("A"));
        break;
    case PdfView::Tool::Underline:
    case PdfView::Tool::StrikeOut: {
        p.setPen(ink);
        p.drawText(box.adjusted(0, -4, 0, -4), Qt::AlignCenter, QStringLiteral("A"));
        const int y = tool == PdfView::Tool::Underline ? 57 : 30;
        p.setPen(QPen(color, 5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(8, y, 56, y);
        break;
    }
    case PdfView::Tool::Note: {
        // A speech bubble.
        QPainterPath path;
        path.addRoundedRect(QRectF(6, 8, 52, 38), 9, 9);
        path.moveTo(18, 45);
        path.lineTo(18, 58);
        path.lineTo(32, 45);
        p.setPen(QPen(QColor(120, 95, 0), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(path.simplified());
        break;
    }
    case PdfView::Tool::Text:
        p.setPen(ink);
        p.drawText(box, Qt::AlignCenter, QStringLiteral("T"));
        p.setPen(QPen(ink, 3));
        p.drawLine(52, 12, 52, 52); // text cursor
        break;
    }
    return QIcon(pixmap);
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    unsavedPrompt = [this](const QString &fileName) {
        return QMessageBox::question(this, tr("Unsaved changes"), tr("“%1” has unsaved annotations. Save them before closing?").arg(fileName),
                                     QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    };
    // Captured once per process, before dark mode can have replaced it.
    static const QPalette startupPalette = QApplication::palette();
    m_lightPalette = startupPalette;
    setAcceptDrops(true);
    resize(1280, 860);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    for (int i = 0; i < 2; ++i) {
        auto *wrapper = new QWidget(m_splitter);
        auto *layout = new QVBoxLayout(wrapper);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // Thin strip marking which pane keyboard commands apply to.
        auto *indicator = new QFrame(wrapper);
        indicator->setFixedHeight(2);
        layout->addWidget(indicator);

        auto *pane = new TabPane(wrapper);
        pane->setObjectName(i == 0 ? QStringLiteral("leftPane") : QStringLiteral("rightPane"));
        layout->addWidget(pane);

        connect(pane, &QTabWidget::tabCloseRequested, this, [this, i](int index) {
            closeTab(i, index);
        });
        connect(pane, &QTabWidget::currentChanged, this, &MainWindow::updateUi);
        connect(pane, &TabPane::tabContextMenuRequested, this, [this, i](int index, const QPoint &pos) {
            showTabMenu(i, index, pos);
        });
        connect(pane, &QTabWidget::tabBarClicked, this, [this, i] {
            setActivePane(i);
        });

        m_splitter->addWidget(wrapper);
        m_panes[i] = pane;
        m_wrappers[i] = wrapper;
        m_indicators[i] = indicator;
    }
    m_wrappers[1]->hide();
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_splitter, 1);
    m_findBar = new FindBar(central);
    centralLayout->addWidget(m_findBar);
    setCentralWidget(central);

    connect(m_findBar, &FindBar::queryChanged, this, &MainWindow::syncSearch);
    connect(m_findBar, &FindBar::findNext, this, [this] {
        if (PdfView *v = currentView()) {
            v->findNext(true);
        }
    });
    connect(m_findBar, &FindBar::findPrevious, this, [this] {
        if (PdfView *v = currentView()) {
            v->findNext(false);
        }
    });
    connect(m_findBar, &FindBar::closed, this, [this] {
        syncSearch();
        if (PdfView *v = currentView()) {
            v->setFocus();
        }
    });

    m_pageLabel = new QLabel(this);
    m_zoomLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_pageLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);

    // Whichever pane holds keyboard focus is the active one.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (QWidget *w = now; w; w = w->parentWidget()) {
            for (int i = 0; i < 2; ++i) {
                if (w == m_panes[i]) {
                    setActivePane(i);
                    return;
                }
            }
        }
    });

    createActions();
    createToolActions();

    // Dark mode is remembered; the first start follows the desktop's scheme.
    const bool systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    setDarkMode(appSettings().value(QStringLiteral("darkMode"), systemDark).toBool());
    updateUi();
}

void MainWindow::createActions()
{
    const auto add = [this](QMenu *menu, const QString &text, const QList<QKeySequence> &keys, auto &&slot) {
        QAction *action = menu->addAction(text, this, std::forward<decltype(slot)>(slot));
        action->setShortcuts(keys);
        return action;
    };
    const auto withView = [this](auto &&fn) {
        return [this, fn] {
            if (PdfView *v = currentView()) {
                fn(v);
            }
        };
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    add(file, tr("&Open…"), {QKeySequence::Open, QKeySequence(Qt::CTRL | Qt::Key_T)}, [this] { openDialog(); });
    m_saveAction = add(file, tr("&Save"), {QKeySequence::Save}, withView([this](PdfView *v) { saveDocument(v->document()); }));
    m_viewActions << add(file, tr("Save &As…"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)}, [this] { saveAs(); });
    file->addSeparator();
    m_viewActions << add(file, tr("&Close Tab"), {QKeySequence(Qt::CTRL | Qt::Key_W)}, [this] {
        closeTab(m_active, m_panes[m_active]->currentIndex());
    });
    file->addSeparator();
    add(file, tr("&Quit"), {QKeySequence(Qt::CTRL | Qt::Key_Q)}, [this] { close(); });

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = add(edit, tr("&Undo"), {QKeySequence::Undo}, withView([](PdfView *v) { v->document()->undoStack()->undo(); }));
    m_redoAction = add(edit, tr("&Redo"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)},
                       withView([](PdfView *v) { v->document()->undoStack()->redo(); }));
    edit->addSeparator();
    m_copyAction = add(edit, tr("&Copy"), {QKeySequence::Copy}, withView([](PdfView *v) { v->copySelection(); }));
    m_viewActions << add(edit, tr("&Find…"), {QKeySequence::Find}, [this] { openFind(); });
    m_viewActions << add(edit, tr("Find &Next"), {QKeySequence(Qt::Key_F3)}, withView([](PdfView *v) { v->findNext(true); }));
    m_viewActions << add(edit, tr("Find &Previous"), {QKeySequence(Qt::SHIFT | Qt::Key_F3)}, withView([](PdfView *v) { v->findNext(false); }));
    edit->addSeparator();
    m_deleteAction = add(edit, tr("&Delete Annotation"), {QKeySequence(Qt::Key_Delete)},
                         withView([](PdfView *v) { v->deleteSelectedAnnotation(); }));

    QMenu *view = menuBar()->addMenu(tr("&View"));
    m_viewActions << add(view, tr("Zoom &In"), {QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal)},
                         withView([](PdfView *v) { v->zoomIn(); }));
    m_viewActions << add(view, tr("Zoom &Out"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)}, withView([](PdfView *v) { v->zoomOut(); }));
    m_viewActions << add(view, tr("&Actual Size"), {QKeySequence(Qt::CTRL | Qt::Key_0)}, withView([](PdfView *v) { v->setZoom(1.0); }));
    m_fitWidthAction = add(view, tr("Fit &Width"), {QKeySequence(Qt::CTRL | Qt::Key_1)},
                           withView([](PdfView *v) { v->setZoomMode(PdfView::ZoomMode::FitWidth); }));
    m_fitPageAction = add(view, tr("Fit &Page"), {QKeySequence(Qt::CTRL | Qt::Key_2)},
                          withView([](PdfView *v) { v->setZoomMode(PdfView::ZoomMode::FitPage); }));
    m_fitWidthAction->setCheckable(true);
    m_fitPageAction->setCheckable(true);
    m_viewActions << m_fitWidthAction << m_fitPageAction;
    view->addSeparator();
    m_darkAction = add(view, tr("&Dark Mode"), {QKeySequence(Qt::CTRL | Qt::Key_D)}, [this] { setDarkMode(!m_dark); });
    m_darkAction->setCheckable(true);
    view->addSeparator();
    m_splitAction = add(view, tr("&Split View"), {QKeySequence(Qt::Key_F8)}, [this] { toggleSplit(); });
    m_splitAction->setCheckable(true);
    m_viewActions << add(view, tr("&Move Tab to Other Pane"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)}, [this] {
        moveTab(m_active, m_panes[m_active]->currentIndex());
    });
    m_splitOnlyActions << add(view, tr("Switch to Other &Pane"), {QKeySequence(Qt::Key_F6)}, [this] { focusOtherPane(); });

    QMenu *go = menuBar()->addMenu(tr("&Go"));
    m_viewActions << add(go, tr("&Next Tab"), {QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)},
                         [this] { switchTab(+1); });
    m_viewActions << add(go, tr("&Previous Tab"),
                         {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
                         [this] { switchTab(-1); });
    go->addSeparator();
    m_viewActions << add(go, tr("&Go to Page…"), {QKeySequence(Qt::CTRL | Qt::Key_G)}, [this] { goToPageDialog(); });
    m_viewActions << add(go, tr("&First Page"), {QKeySequence(Qt::CTRL | Qt::Key_Home)}, withView([](PdfView *v) { v->goToPage(0); }));
    m_viewActions << add(go, tr("&Last Page"), {QKeySequence(Qt::CTRL | Qt::Key_End)},
                         withView([](PdfView *v) { v->goToPage(v->pageCount() - 1); }));
}

// --- tools -------------------------------------------------------------------

void MainWindow::createToolActions()
{
    using Tool = PdfView::Tool;
    struct Def {
        QString text;
        Tool tool;
        QColor color;
    };
    QList<Def> defs{{tr("&Select"), Tool::Select, QColor()}};
    for (const auto &[name, color] : PdfView::highlightColors()) {
        defs.append({tr("Highlight %1").arg(name), Tool::Highlight, color});
    }
    defs.append({tr("&Underline"), Tool::Underline, PdfView::lineColor()});
    defs.append({tr("Stri&ke Out"), Tool::StrikeOut, PdfView::lineColor()});
    defs.append({tr("&Comment"), Tool::Note, PdfView::noteColor()});
    defs.append({tr("&Text"), Tool::Text, QColor(Qt::black)});

    QMenu *menu = new QMenu(tr("&Tools"), this);
    menuBar()->insertMenu(menuBar()->actions().at(2), menu); // after Edit
    QToolBar *bar = addToolBar(tr("Tools"));
    bar->setObjectName(QStringLiteral("toolsBar"));
    bar->setMovable(false);
    bar->setIconSize(QSize(22, 22));
    auto *group = new QActionGroup(this);

    int number = 1;
    for (const Def &def : std::as_const(defs)) {
        // Alt+1 … Alt+9, in toolbar order.
        const QKeySequence key(Qt::ALT | Qt::Key(Qt::Key_0 + number++));
        QAction *action = menu->addAction(def.text);
        action->setShortcut(key);
        action->setCheckable(true);
        action->setToolTip(QStringLiteral("%1 (%2)").arg(def.text).remove(QLatin1Char('&')).arg(key.toString(QKeySequence::NativeText)));
        group->addAction(action);
        bar->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool = def.tool, color = def.color] {
            triggerTool(tool, color);
        });
        m_toolActions.append({action, {def.tool, def.color}});
        if (def.tool == Tool::Select || def.tool == Tool::StrikeOut) {
            menu->addSeparator();
            bar->addSeparator();
        }
    }
}

// The icons are drawn in the text colour, which changes with dark mode.
void MainWindow::updateToolIcons()
{
    const QColor ink = QApplication::palette().color(QPalette::WindowText);
    for (const auto &[action, def] : std::as_const(m_toolActions)) {
        action->setIcon(toolIcon(def.first, def.second, ink));
    }
}

// --- dark mode and search ------------------------------------------------------

void MainWindow::setDarkMode(bool dark)
{
    m_dark = dark;
    QApplication::setPalette(dark ? darkPalette() : m_lightPalette);
    for (const TabPane *pane : m_panes) {
        for (int i = 0; i < pane->count(); ++i) {
            pane->view(i)->setDarkMode(dark);
        }
    }
    updateToolIcons();
    m_darkAction->setChecked(dark);
    appSettings().setValue(QStringLiteral("darkMode"), dark);
}

void MainWindow::openFind()
{
    const PdfView *v = currentView();
    if (!v) {
        return;
    }
    // Searching for the selected text is the common case.
    QString selected = v->selectedText();
    if (selected.contains(QLatin1Char('\n'))) {
        selected.clear();
    }
    m_findBar->open(selected);
}

// Makes the current view search for what the find bar says. Other tabs keep
// their own matches until they become current.
void MainWindow::syncSearch()
{
    if (PdfView *v = currentView()) {
        v->setSearch(m_findBar->isVisible() ? m_findBar->text() : QString(), m_findBar->caseSensitive());
    }
    updateUi();
}

void MainWindow::triggerTool(PdfView::Tool tool, const QColor &color)
{
    using Tool = PdfView::Tool;
    // With text already selected, a markup shortcut marks that text and
    // leaves the current tool alone.
    PdfView *v = currentView();
    if (v && v->hasTextSelection() && (tool == Tool::Highlight || tool == Tool::Underline || tool == Tool::StrikeOut)) {
        const auto kind = tool == Tool::Highlight ? AnnotationData::Kind::Highlight
            : tool == Tool::Underline ? AnnotationData::Kind::Underline
                                      : AnnotationData::Kind::StrikeOut;
        v->markupSelection(kind, color);
        updateUi(); // restores the checked tool
        return;
    }
    setTool(tool, color);
}

void MainWindow::setTool(PdfView::Tool tool, const QColor &color)
{
    m_tool = tool;
    m_toolColor = color;
    for (const TabPane *pane : m_panes) {
        for (int i = 0; i < pane->count(); ++i) {
            pane->view(i)->setTool(tool, color);
        }
    }
    updateUi();
}

// --- saving ------------------------------------------------------------------

bool MainWindow::saveDocument(const std::shared_ptr<Document> &doc)
{
    QString error;
    if (!doc->save(doc->path(), &error)) {
        QMessageBox::warning(this, tr("Cannot save"), error);
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(doc->fileName()), 3000);
    return true;
}

void MainWindow::saveAs()
{
    const PdfView *v = currentView();
    if (!v) {
        return;
    }
    const std::shared_ptr<Document> doc = v->document();
    QString path = QFileDialog::getSaveFileName(this, tr("Save As"), doc->path(), tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".pdf");
    }
    QString error;
    if (!doc->save(path, &error)) {
        QMessageBox::warning(this, tr("Cannot save"), error);
    }
}

int MainWindow::viewCount(const std::shared_ptr<Document> &doc) const
{
    int n = 0;
    for (const TabPane *pane : m_panes) {
        for (int i = 0; i < pane->count(); ++i) {
            n += pane->view(i)->document() == doc;
        }
    }
    return n;
}

// True if the document may be closed (saved, discarded, or nothing to lose).
bool MainWindow::confirmClose(const std::shared_ptr<Document> &doc)
{
    if (!doc->isModified()) {
        return true;
    }
    switch (unsavedPrompt(doc->fileName())) {
    case QMessageBox::Save:
        return saveDocument(doc);
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QList<std::shared_ptr<Document>> asked;
    for (const TabPane *pane : m_panes) {
        for (int i = 0; i < pane->count(); ++i) {
            const std::shared_ptr<Document> doc = pane->view(i)->document();
            if (asked.contains(doc)) {
                continue;
            }
            asked.append(doc);
            if (!confirmClose(doc)) {
                event->ignore();
                return;
            }
        }
    }
    event->accept();
}

// --- opening ---------------------------------------------------------------

std::shared_ptr<Document> MainWindow::findOpenDocument(const QString &path) const
{
    for (const TabPane *pane : m_panes) {
        const int index = pane->indexOfPath(path);
        if (index >= 0) {
            return pane->view(index)->document();
        }
    }
    return nullptr;
}

std::shared_ptr<Document> MainWindow::loadDocument(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    QString password;
    for (;;) {
        Document::LoadStatus status;
        std::shared_ptr<Document> doc = Document::load(path, password, &status);
        if (status == Document::LoadStatus::Ok) {
            return doc;
        }
        if (status == Document::LoadStatus::Failed) {
            QMessageBox::warning(this, tr("Cannot open file"), tr("“%1” could not be opened as a PDF.").arg(name));
            return nullptr;
        }
        bool ok = false;
        password = QInputDialog::getText(this, tr("Password required"), tr("“%1” is encrypted. Password:").arg(name),
                                         QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return nullptr;
        }
    }
}

bool MainWindow::openFile(const QString &path, int pane)
{
    if (pane < 0) {
        pane = m_active;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();

    const int existing = m_panes[pane]->indexOfPath(absolute);
    if (existing >= 0) {
        m_panes[pane]->setCurrentIndex(existing);
        m_panes[pane]->currentView()->setFocus();
        return true;
    }

    std::shared_ptr<Document> doc = findOpenDocument(absolute);
    if (!doc) {
        doc = loadDocument(absolute);
    }
    if (!doc) {
        return false;
    }
    addView(new PdfView(doc), pane);
    return true;
}

void MainWindow::openFiles(const QStringList &paths, int pane)
{
    for (const QString &path : paths) {
        openFile(path, pane);
    }
}

void MainWindow::openDialog()
{
    QString dir;
    if (const PdfView *v = currentView()) {
        dir = QFileInfo(v->document()->path()).absolutePath();
    }
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open PDF"), dir, tr("PDF documents (*.pdf);;All files (*)"));
    openFiles(files);
}

void MainWindow::addView(PdfView *view, int pane)
{
    // Moving a tab between panes comes through here again; connect only once.
    if (!view->property("connected").toBool()) {
        view->setProperty("connected", true);
        Document *doc = view->document().get();
        connect(view, &PdfView::currentPageChanged, this, &MainWindow::updateUi);
        connect(view, &PdfView::zoomChanged, this, &MainWindow::updateUi);
        connect(view, &PdfView::selectionChanged, this, &MainWindow::updateUi);
        connect(view, &PdfView::searchChanged, this, &MainWindow::updateUi);
        view->setDarkMode(m_dark);
        connect(view, &PdfView::toolFinished, this, [this] {
            setTool(PdfView::Tool::Select, QColor());
        });
        // The view is the context so these go away with the tab.
        connect(doc, &Document::modifiedChanged, view, [this] { updateUi(); });
        connect(doc, &Document::pathChanged, view, [this] { updateUi(); });
        connect(doc->undoStack(), &QUndoStack::indexChanged, view, [this] { updateUi(); });
        view->setTool(m_tool, m_toolColor);
    }

    const bool wasSplit = isSplit();
    const int index = m_panes[pane]->addView(view);
    m_panes[pane]->setCurrentIndex(index);
    normalizePanes();
    if (!wasSplit && isSplit()) {
        const int half = m_splitter->width() / 2;
        m_splitter->setSizes({half, half});
    }
    // normalizePanes() may have moved the view to the left pane.
    for (int i = 0; i < 2; ++i) {
        if (m_panes[i]->indexOf(view) >= 0) {
            setActivePane(i);
        }
    }
    view->setFocus();
    updateUi();
}

// --- tabs and panes --------------------------------------------------------

bool MainWindow::isSplit() const
{
    return m_panes[1]->count() > 0;
}

PdfView *MainWindow::currentView() const
{
    return m_panes[m_active]->currentView();
}

void MainWindow::closeTab(int pane, int index)
{
    PdfView *view = m_panes[pane]->view(index);
    if (!view) {
        return;
    }
    // Only the last tab showing a document can lose its changes.
    if (viewCount(view->document()) == 1 && !confirmClose(view->document())) {
        return;
    }
    m_panes[pane]->removeTab(index);
    view->deleteLater();
    normalizePanes();
    if (PdfView *next = currentView()) {
        next->setFocus();
    }
    updateUi();
}

void MainWindow::moveTab(int fromPane, int index)
{
    PdfView *view = m_panes[fromPane]->view(index);
    if (!view) {
        return;
    }
    // Moving the only tab of an unsplit window would just bounce it back.
    if (!isSplit() && m_panes[0]->count() < 2) {
        return;
    }
    m_panes[fromPane]->removeTab(index);
    addView(view, 1 - fromPane);
}

void MainWindow::duplicateTab(int fromPane, int index)
{
    const PdfView *source = m_panes[fromPane]->view(index);
    if (!source) {
        return;
    }
    auto *copy = new PdfView(source->document());
    copy->restoreViewState(source->viewState());
    addView(copy, 1 - fromPane);
}

void MainWindow::toggleSplit()
{
    if (isSplit()) {
        // Merge everything back into the left pane, keeping the focused tab current.
        PdfView *keep = currentView();
        while (m_panes[1]->count() > 0) {
            PdfView *view = m_panes[1]->view(0);
            m_panes[1]->removeTab(0);
            m_panes[0]->addView(view);
        }
        normalizePanes();
        if (keep) {
            m_panes[0]->setCurrentWidget(keep);
            keep->setFocus();
        }
        updateUi();
    } else if (m_panes[0]->currentView()) {
        duplicateTab(0, m_panes[0]->currentIndex());
    } else {
        updateUi(); // nothing to split; un-check the action again
    }
}

// The left pane is the primary one: the right pane is only shown while it has
// tabs, and if the left one runs empty the right one's tabs take its place.
void MainWindow::normalizePanes()
{
    if (m_panes[0]->count() == 0 && m_panes[1]->count() > 0) {
        PdfView *current = m_panes[1]->currentView();
        while (m_panes[1]->count() > 0) {
            PdfView *view = m_panes[1]->view(0);
            m_panes[1]->removeTab(0);
            m_panes[0]->addView(view);
        }
        m_panes[0]->setCurrentWidget(current);
    }
    m_wrappers[1]->setVisible(isSplit());
    if (!isSplit()) {
        m_active = 0;
    }
}

void MainWindow::setActivePane(int pane)
{
    if (pane == 1 && !isSplit()) {
        pane = 0;
    }
    if (pane != m_active) {
        m_active = pane;
        updateUi();
    }
}

void MainWindow::focusOtherPane()
{
    if (!isSplit()) {
        return;
    }
    setActivePane(1 - m_active);
    if (PdfView *v = currentView()) {
        v->setFocus();
    }
}

void MainWindow::switchTab(int delta)
{
    TabPane *pane = m_panes[m_active];
    const int n = pane->count();
    if (n > 1) {
        pane->setCurrentIndex((pane->currentIndex() + delta + n) % n);
        pane->currentView()->setFocus();
    }
}

void MainWindow::showTabMenu(int pane, int index, const QPoint &globalPos)
{
    QMenu menu(this);
    const bool canMove = isSplit() || m_panes[0]->count() > 1;
    menu.addAction(isSplit() ? tr("Open in Other Pane") : tr("Open in Split View"), this, [=, this] { duplicateTab(pane, index); });
    menu.addAction(isSplit() ? tr("Move to Other Pane") : tr("Move to Split View"), this, [=, this] { moveTab(pane, index); })
        ->setEnabled(canMove);
    menu.addSeparator();
    menu.addAction(tr("Close Tab"), this, [=, this] { closeTab(pane, index); });
    menu.addAction(tr("Close Other Tabs"), this, [=, this] {
        PdfView *keep = m_panes[pane]->view(index);
        for (int i = m_panes[pane]->count() - 1; i >= 0; --i) {
            if (m_panes[pane]->view(i) != keep) {
                closeTab(pane, i);
            }
        }
    })->setEnabled(m_panes[pane]->count() > 1);
    menu.exec(globalPos);
}

void MainWindow::goToPageDialog()
{
    PdfView *v = currentView();
    if (!v) {
        return;
    }
    bool ok = false;
    const int page = QInputDialog::getInt(this, tr("Go to Page"), tr("Page (1–%1):").arg(v->pageCount()), v->currentPage() + 1, 1,
                                          v->pageCount(), 1, &ok);
    if (ok) {
        v->goToPage(page - 1);
    }
}

bool MainWindow::isRendering() const
{
    for (const TabPane *pane : m_panes) {
        const PdfView *v = pane->currentView();
        if (v && v->isRendering()) {
            return true;
        }
    }
    return false;
}

void MainWindow::updateUi()
{
    const PdfView *v = currentView();
    const bool split = isSplit();

    setWindowTitle(v ? tr("%1%2 — yap-dfv").arg(v->document()->fileName(), v->document()->isModified() ? QStringLiteral(" *") : QString())
                     : tr("yap-dfv"));
    m_pageLabel->setText(v ? tr("Page %1 of %2").arg(v->currentPage() + 1).arg(v->pageCount()) : QString());
    m_zoomLabel->setText(v ? tr("%1 %").arg(qRound(v->zoom() * 100)) : QString());

    for (int i = 0; i < 2; ++i) {
        const bool highlight = split && i == m_active;
        m_indicators[i]->setStyleSheet(highlight ? QStringLiteral("background: palette(highlight);") : QString());

        // Tab titles follow the document: renamed by Save As, starred while unsaved.
        for (int t = 0; t < m_panes[i]->count(); ++t) {
            const std::shared_ptr<Document> doc = m_panes[i]->view(t)->document();
            // "&" would otherwise be read as a keyboard mnemonic.
            const QString name = doc->fileName().replace(QLatin1Char('&'), QStringLiteral("&&"));
            m_panes[i]->setTabText(t, name + (doc->isModified() ? QStringLiteral(" *") : QString()));
            m_panes[i]->setTabToolTip(t, doc->path());
        }
    }

    // The find bar follows the current view, e.g. after switching tabs.
    if (m_findBar->isVisible()) {
        if (!v) {
            m_findBar->setStatus(QString(), false);
        } else if (v->searchText() != m_findBar->text()) {
            currentView()->setSearch(m_findBar->text(), m_findBar->caseSensitive());
        }
        if (v) {
            const int count = v->matchCount();
            const QString more = v->isSearching() ? QStringLiteral("+") : QString();
            if (v->searchText().isEmpty()) {
                m_findBar->setStatus(QString(), false);
            } else if (count == 0) {
                m_findBar->setStatus(v->isSearching() ? tr("Searching…") : tr("No matches"), !v->isSearching());
            } else {
                m_findBar->setStatus(tr("%1 of %2%3").arg(v->currentMatch() + 1).arg(count).arg(more), false);
            }
        }
    }

    Document *doc = v ? v->document().get() : nullptr;
    m_undoAction->setEnabled(doc && doc->undoStack()->canUndo());
    m_redoAction->setEnabled(doc && doc->undoStack()->canRedo());
    m_saveAction->setEnabled(doc && doc->isModified());
    m_copyAction->setEnabled(v && v->hasTextSelection());
    m_deleteAction->setEnabled(v && !v->selectedAnnotation().isEmpty());
    for (const auto &[action, def] : std::as_const(m_toolActions)) {
        action->setChecked(def.first == m_tool && def.second == m_toolColor);
    }

    for (QAction *action : std::as_const(m_viewActions)) {
        action->setEnabled(v != nullptr);
    }
    for (QAction *action : std::as_const(m_splitOnlyActions)) {
        action->setEnabled(split);
    }
    m_splitAction->setEnabled(split || v != nullptr);
    m_splitAction->setChecked(split);
    m_fitWidthAction->setChecked(v && v->zoomMode() == PdfView::ZoomMode::FitWidth);
    m_fitPageAction->setChecked(v && v->zoomMode() == PdfView::ZoomMode::FitPage);
}

// --- drag and drop ---------------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    // Dropping on the right half of a split window opens there.
    int pane = m_active;
    if (isSplit()) {
        pane = m_wrappers[1]->geometry().contains(m_splitter->mapFrom(this, event->position().toPoint())) ? 1 : 0;
    }
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            openFile(url.toLocalFile(), pane);
        }
    }
    event->acceptProposedAction();
}
