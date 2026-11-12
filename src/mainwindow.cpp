#include "mainwindow.h"

#include "document.h"
#include "pdfview.h"
#include "tabpane.h"

#include <QActionGroup>
#include <QApplication>
#include <QDragEnterEvent>
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
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
    setCentralWidget(m_splitter);

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
    m_viewActions << add(file, tr("&Close Tab"), {QKeySequence(Qt::CTRL | Qt::Key_W)}, [this] {
        closeTab(m_active, m_panes[m_active]->currentIndex());
    });
    file->addSeparator();
    add(file, tr("&Quit"), {QKeySequence(Qt::CTRL | Qt::Key_Q)}, [this] { close(); });

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
    connect(view, &PdfView::currentPageChanged, this, &MainWindow::updateUi);
    connect(view, &PdfView::zoomChanged, this, &MainWindow::updateUi);

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

    setWindowTitle(v ? tr("%1 — Okular++").arg(v->document()->fileName()) : tr("Okular++"));
    m_pageLabel->setText(v ? tr("Page %1 of %2").arg(v->currentPage() + 1).arg(v->pageCount()) : QString());
    m_zoomLabel->setText(v ? tr("%1 %").arg(qRound(v->zoom() * 100)) : QString());

    for (int i = 0; i < 2; ++i) {
        const bool highlight = split && i == m_active;
        m_indicators[i]->setStyleSheet(highlight ? QStringLiteral("background: palette(highlight);") : QString());
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
