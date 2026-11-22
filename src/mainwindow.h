#pragma once

#include "pdfview.h"

#include <QMainWindow>
#include <QMessageBox>

#include <QJsonObject>
#include <QPointer>

#include <array>
#include <functional>
#include <memory>
#include <optional>

class FindBar;
class QAction;
class QFrame;
class QLabel;
class QMenu;
class QMimeData;
class QRubberBand;
class QSplitter;
class StartPage;
class TabPane;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // pane: 0 = left, 1 = right, -1 = the active one.
    bool openFile(const QString &path, int pane = -1);
    // Like openFile(), at a given tab position (-1 appends) and, if given,
    // at a given place in the document.
    bool openTab(const QString &path, int pane, int insertAt, const std::optional<PdfView::ViewState> &state);
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
    // Asked before a document with unsaved changes moves to a window of its
    // own, which reads it from disk; true means saved. Replaceable for tests.
    std::function<bool(const std::shared_ptr<Document> &doc)> saveBeforeDetach;
    // Asks which pages to act on, as text like "1-3, 5"; empty means
    // cancelled. Replaceable for tests.
    std::function<QString(const QString &title, const QString &label, const QString &text)> pageRangePrompt;
    // Asks where extracted pages go; empty means cancelled. Replaceable for tests.
    std::function<QString(const QString &suggested)> extractPathPrompt;
    // Asks which pages to turn and how far (90, 180 or 270 degrees
    // clockwise), starting from `text` and `degrees`; `problem` says what was
    // wrong with the last answer. Nothing means cancelled. Replaceable for tests.
    std::function<std::optional<std::pair<QString, int>>(const QString &problem, const QString &text, int degrees)> rotatePrompt;

    // Starts another yap-dfe process with these arguments. Replaceable so
    // tests can see the arguments instead.
    static std::function<bool(const QStringList &arguments)> startInstance;

    // Tells this window apart from others, also in other processes, in the
    // payload of a dragged tab.
    QString windowId() const { return m_id; }

    // A view's place as JSON, for the session and for tabs moving between windows.
    static QJsonObject viewStateToJson(const PdfView::ViewState &state);
    static PdfView::ViewState viewStateFromJson(const QJsonObject &json);

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
    QStringList recentFiles() const;
    void setRecentFiles(const QStringList &paths);
    void noteRecentFile(const QString &path);
    void fillRecentMenu();
    void closeTab(int pane, int index);
    void discardView(PdfView *view); // removes a tab without asking anything
    void dragTab(int pane, int index);
    void detachTab(PdfView *view);
    void openLinkedFile(const QString &path, int page);
    void moveTab(int fromPane, int index, int insertAt = -1);
    // What dropping a dragged tab at a point would do.
    struct TabDrop {
        bool ownWindow = false; // false: the tab comes from another window
        int fromPane = 0; // own window only
        int index = 0; //
        QString path; // other window only
        bool modified = false; //
        PdfView::ViewState state; //
        int toPane = 0;
        int insertAt = -1;
        QRect area; // highlighted while dragging
    };
    bool tabDropTarget(const QMimeData *mime, const QPoint &pos, TabDrop *drop) const;
    void saveSession();
    void applyDarkPages();
    void duplicateTab(int fromPane, int index);
    void showTabMenu(int pane, int index, const QPoint &globalPos);
    void switchTab(int delta);
    void focusOtherPane();
    void goToPageDialog();
    std::optional<QList<int>> askPages(PdfView *v, const QString &title, const QString &label);
    void removePages(PdfView *v, const QList<int> &pages);
    void extractPages(PdfView *v, const QList<int> &pages);
    void rotatePages(PdfView *v, const QList<int> &pages, int degrees);
    void rotatePagesDialog(PdfView *v);
    void removeSelectedText(PdfView *v);
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

    StartPage *m_startPage = nullptr; // fills the left pane while nothing is open
    QMenu *m_recentMenu = nullptr;
    bool m_restoring = false; // reopening last session's tabs, which is not "recent" use
    FindBar *m_findBar = nullptr;
    bool m_dark = false;
    QPalette m_lightPalette; // the palette the application started with
    QAction *m_darkAction = nullptr;
    bool m_darkPages = true; // in dark mode, darken the pages too
    QAction *m_darkPagesAction = nullptr;
    QAction *m_restoreAction = nullptr;
    QRubberBand *m_dropIndicator = nullptr;

    QString m_id;
    // While one of this window's tabs is being dragged.
    struct {
        bool active = false;
        bool inside = true; // the pointer is over this window
        bool droppedHere = false;
    } m_tabDrag;

    QLabel *m_pageLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;

    PdfView::Tool m_tool = PdfView::Tool::Select;
    QColor m_toolColor;
    QList<QPair<QAction *, QPair<PdfView::Tool, QColor>>> m_toolActions;

    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_removeTextAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_splitAction = nullptr;
    QAction *m_fitWidthAction = nullptr;
    QAction *m_fitPageAction = nullptr;
    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QList<QAction *> m_viewActions; // enabled only while a document is open
    QList<QAction *> m_splitOnlyActions;
};
