#pragma once

#include "pdfview.h"

#include <QMainWindow>
#include <QMessageBox>

#include <array>
#include <functional>
#include <memory>

class FindBar;
class QAction;
class QFrame;
class QLabel;
class QMimeData;
class QRubberBand;
class QSplitter;
class TabPane;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // pane: 0 = left, 1 = right, -1 = the active one.
    bool openFile(const QString &path, int pane = -1);
    void openFiles(const QStringList &paths, int pane = -1);
    // What a launch asks for: the files as tabs in the active pane; with
    // `split`, the last one in the right pane (a single file in both).
    void openRequest(const QStringList &paths, bool split);
    // Brings the window to the front on behalf of another launch.
    void activate(const QByteArray &activationToken);
    void toggleSplit();
    bool isRendering() const;

    PdfView::Tool tool() const { return m_tool; }

    bool darkMode() const { return m_dark; }
    void setDarkMode(bool dark);

    // Reopens the tabs, split and positions saved when the window last
    // closed. Does nothing if the user switched that off.
    void restoreSession();

    // Asked before a document with unsaved changes is closed; answers Save,
    // Discard or Cancel. Replaceable so tests can answer without a dialog.
    std::function<QMessageBox::StandardButton(const QString &fileName)> unsavedPrompt;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();
    void createToolActions();
    void updateToolIcons();
    void openFind();
    void syncSearch();
    void triggerTool(PdfView::Tool tool, const QColor &color);
    void setTool(PdfView::Tool tool, const QColor &color);
    bool saveDocument(const std::shared_ptr<Document> &doc);
    void saveAs();
    bool confirmClose(const std::shared_ptr<Document> &doc);
    int viewCount(const std::shared_ptr<Document> &doc) const;
    void openDialog();
    void closeTab(int pane, int index);
    void moveTab(int fromPane, int index, int insertAt = -1);
    bool tabDropTarget(const QMimeData *mime, const QPoint &pos, int *fromPane, int *index, int *toPane, int *insertAt, QRect *area) const;
    void saveSession();
    void applyDarkPages();
    void duplicateTab(int fromPane, int index);
    void showTabMenu(int pane, int index, const QPoint &globalPos);
    void switchTab(int delta);
    void focusOtherPane();
    void goToPageDialog();
    void setActivePane(int pane);
    void addView(PdfView *view, int pane, int insertAt = -1);
    void normalizePanes();
    void updateUi();

    bool isSplit() const;
    PdfView *currentView() const;
    std::shared_ptr<Document> findOpenDocument(const QString &path) const;
    std::shared_ptr<Document> loadDocument(const QString &path);

    QSplitter *m_splitter = nullptr;
    std::array<TabPane *, 2> m_panes{};
    std::array<QWidget *, 2> m_wrappers{};
    std::array<QFrame *, 2> m_indicators{};
    int m_active = 0;

    FindBar *m_findBar = nullptr;
    bool m_dark = false;
    QPalette m_lightPalette; // the palette the application started with
    QAction *m_darkAction = nullptr;
    bool m_darkPages = true; // in dark mode, darken the pages too
    QAction *m_darkPagesAction = nullptr;
    QAction *m_restoreAction = nullptr;
    QRubberBand *m_dropIndicator = nullptr;

    QLabel *m_pageLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;

    PdfView::Tool m_tool = PdfView::Tool::Select;
    QColor m_toolColor;
    QList<QPair<QAction *, QPair<PdfView::Tool, QColor>>> m_toolActions;

    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_splitAction = nullptr;
    QAction *m_fitWidthAction = nullptr;
    QAction *m_fitPageAction = nullptr;
    QList<QAction *> m_viewActions; // enabled only while a document is open
    QList<QAction *> m_splitOnlyActions;
};
