#include "tabpane.h"

#include "document.h"
#include "pdfview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>

namespace {
class TabBar : public QTabBar
{
public:
    using QTabBar::QTabBar;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton) {
            const int index = tabAt(event->position().toPoint());
            if (index >= 0) {
                Q_EMIT tabCloseRequested(index);
            }
            return;
        }
        QTabBar::mouseReleaseEvent(event);
    }
};
}

TabPane::TabPane(QWidget *parent)
    : QTabWidget(parent)
{
    setTabBar(new TabBar(this));
    setDocumentMode(true);
    setMovable(true);
    setTabsClosable(true);
    setElideMode(Qt::ElideMiddle);
    tabBar()->setExpanding(false);
    tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int index = tabBar()->tabAt(pos);
        if (index >= 0) {
            Q_EMIT tabContextMenuRequested(index, tabBar()->mapToGlobal(pos));
        }
    });
}

PdfView *TabPane::view(int index) const
{
    return qobject_cast<PdfView *>(widget(index));
}

PdfView *TabPane::currentView() const
{
    return qobject_cast<PdfView *>(currentWidget());
}

int TabPane::addView(PdfView *view)
{
    const int index = addTab(view, view->document()->fileName().replace(QLatin1Char('&'), QStringLiteral("&&")));
    setTabToolTip(index, view->document()->path());
    return index;
}

int TabPane::indexOfPath(const QString &path) const
{
    for (int i = 0; i < count(); ++i) {
        if (view(i)->document()->path() == path) {
            return i;
        }
    }
    return -1;
}

void TabPane::paintEvent(QPaintEvent *event)
{
    if (count() > 0) {
        QTabWidget::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setPen(palette().color(QPalette::PlaceholderText));
    p.drawText(rect(), Qt::AlignCenter, tr("Open a PDF with Ctrl+O, or drop files here"));
}
