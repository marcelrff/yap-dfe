#include "tabpane.h"

#include "document.h"
#include "pdfview.h"

#include <QDrag>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>

namespace {
// Adds middle-click close, and notices when a tab is dragged out of the bar.
// Inside the bar, Qt's own drag keeps reordering tabs as before.
class TabBar : public QTabBar
{
public:
    TabBar(QTabWidget *owner, std::function<void(QWidget *)> draggedOut)
        : QTabBar(owner)
        , m_owner(owner)
        , m_draggedOut(std::move(draggedOut))
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            // Remember the page, not the index: reordering changes the index.
            m_pressed = m_owner->widget(tabAt(event->position().toPoint()));
        }
        QTabBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint pos = event->position().toPoint();
        if ((event->buttons() & Qt::LeftButton) && m_pressed && !rect().adjusted(-30, -20, 30, 20).contains(pos)) {
            // End Qt's reordering drag cleanly before starting a real one.
            QMouseEvent release(QEvent::MouseButtonRelease, event->position(), event->globalPosition(), Qt::LeftButton, Qt::NoButton,
                                event->modifiers());
            QTabBar::mouseReleaseEvent(&release);
            QWidget *page = m_pressed;
            m_pressed = nullptr;
            m_draggedOut(page);
            return;
        }
        QTabBar::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_pressed = nullptr;
        if (event->button() == Qt::MiddleButton) {
            const int index = tabAt(event->position().toPoint());
            if (index >= 0) {
                Q_EMIT tabCloseRequested(index);
            }
            return;
        }
        QTabBar::mouseReleaseEvent(event);
    }

private:
    QTabWidget *m_owner;
    std::function<void(QWidget *)> m_draggedOut;
    QPointer<QWidget> m_pressed;
};
}

std::function<Qt::DropAction(QDrag *)> TabPane::execDrag = [](QDrag *drag) {
    return drag->exec(Qt::MoveAction);
};

const char *TabPane::tabMimeType()
{
    return "application/x-yap-dfe-tab";
}

TabPane::TabPane(QWidget *parent)
    : QTabWidget(parent)
{
    setTabBar(new TabBar(this, [this](QWidget *page) {
        const int index = indexOf(page);
        if (index >= 0) {
            Q_EMIT tabDragRequested(index);
        }
    }));
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

int TabPane::addView(PdfView *view, int at)
{
    const int index = insertTab(at, view, view->document()->fileName().replace(QLatin1Char('&'), QStringLiteral("&&")));
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

void TabPane::setEmptyWidget(QWidget *widget)
{
    m_emptyWidget = widget;
    widget->setParent(this);
    widget->setGeometry(rect());
    widget->setVisible(count() == 0);
}

void TabPane::resizeEvent(QResizeEvent *event)
{
    QTabWidget::resizeEvent(event);
    if (m_emptyWidget) {
        m_emptyWidget->setGeometry(rect());
    }
}

void TabPane::tabInserted(int index)
{
    QTabWidget::tabInserted(index);
    if (m_emptyWidget) {
        m_emptyWidget->hide();
    }
}

void TabPane::tabRemoved(int index)
{
    QTabWidget::tabRemoved(index);
    if (m_emptyWidget && count() == 0) {
        m_emptyWidget->show();
        m_emptyWidget->raise();
    }
}
