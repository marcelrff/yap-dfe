#pragma once

#include "pdfview.h"

#include <QMainWindow>
#include <QMessageBox>

#include <array>
#include <functional>
#include <memory>

class QAction;
class QFrame;
class QLabel;
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
    void toggleSplit();
    bool isRendering() const;

    PdfView::Tool tool() const { return m_tool; }

    // Asked before a document with unsaved changes is closed; answers Save,
    // Discard or Cancel. Replaceable so tests can answer without a dialog.
    std::function<QMessageBox::StandardButton(const QString &fileName)> unsavedPrompt;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();
    void createToolActions();
    void triggerTool(PdfView::Tool tool, const QColor &color);
    void setTool(PdfView::Tool tool, const QColor &color);
    bool saveDocument(const std::shared_ptr<Document> &doc);
    void saveAs();
    bool confirmClose(const std::shared_ptr<Document> &doc);
    int viewCount(const std::shared_ptr<Document> &doc) const;
    void openDialog();
    void closeTab(int pane, int index);
    void moveTab(int fromPane, int index);
    void duplicateTab(int fromPane, int index);
    void showTabMenu(int pane, int index, const QPoint &globalPos);
    void switchTab(int delta);
    void focusOtherPane();
    void goToPageDialog();
    void setActivePane(int pane);
    void addView(PdfView *view, int pane);
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
