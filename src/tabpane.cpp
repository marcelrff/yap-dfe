#include "tabpane.h"

#include "document.h"
#include "pdfview.h"

#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPainter>
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

std::function<void(QDrag *)> TabPane::execDrag = [](QDrag *drag) {
    drag->exec(Qt::MoveAction);
};

const char *TabPane::tabMimeType()
{
    return "application/x-yap-dfe-tab";
}

void TabPane::startTabDrag(QWidget *page)
{
    const int index = indexOf(page);
    if (index < 0) {
        return;
    }
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(tabMimeType()), objectName().toUtf8() + ':' + QByteArray::number(index));
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(tabBar()->grab(tabBar()->tabRect(index)));
    execDrag(drag);
}

TabPane::TabPane(QWidget *parent)
    : QTabWidget(parent)
{
    setTabBar(new TabBar(this, [this](QWidget *page) {
        startTabDrag(page);
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
