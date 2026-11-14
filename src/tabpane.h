#pragma once

#include <QTabWidget>

#include <functional>

class QDrag;
class PdfView;

// One tab strip plus its views. The main window holds two of these in a
// splitter to provide side-by-side viewing.
class TabPane : public QTabWidget
{
    Q_OBJECT

public:
    explicit TabPane(QWidget *parent = nullptr);

    PdfView *view(int index) const;
    PdfView *currentView() const;
    int addView(PdfView *view, int index = -1); // -1 appends
    int indexOfPath(const QString &path) const;

    // Shown across the pane for as long as it has no tabs. Takes ownership.
    void setEmptyWidget(QWidget *widget);

    // A tab dragged out of its bar travels as this MIME type, between
    // windows too. The main window fills in the payload and handles the drop.
    static const char *tabMimeType();
    // Runs the drag and returns what the drop target did. Replaceable so
    // tests can play the target without a real drag-and-drop session.
    static std::function<Qt::DropAction(QDrag *)> execDrag;

Q_SIGNALS:
    void tabContextMenuRequested(int index, const QPoint &globalPos);
    // The tab left the bar with the mouse button held: time for a real drag.
    void tabDragRequested(int index);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;

private:
    QWidget *m_emptyWidget = nullptr;
};
