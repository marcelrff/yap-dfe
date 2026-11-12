#pragma once

#include <QMainWindow>

#include <array>
#include <memory>

class Document;
class PdfView;
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

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void createActions();
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

    QAction *m_splitAction = nullptr;
    QAction *m_fitWidthAction = nullptr;
    QAction *m_fitPageAction = nullptr;
    QList<QAction *> m_viewActions; // enabled only while a document is open
    QList<QAction *> m_splitOnlyActions;
};
