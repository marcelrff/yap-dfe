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

    // A tab dragged out of its bar travels as this MIME type; the payload is
    // "<pane object name>:<tab index>". The main window handles the drop.
    static const char *tabMimeType();
    // Runs the drag. Replaceable so tests can observe it without a real
    // drag-and-drop session.
    static std::function<void(QDrag *)> execDrag;

Q_SIGNALS:
    void tabContextMenuRequested(int index, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void startTabDrag(QWidget *page);
};
