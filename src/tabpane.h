#pragma once

#include <QTabWidget>

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
    int addView(PdfView *view);
    int indexOfPath(const QString &path) const;

Q_SIGNALS:
    void tabContextMenuRequested(int index, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *event) override;
};
