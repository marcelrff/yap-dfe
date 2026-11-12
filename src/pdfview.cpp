#include "pdfview.h"

#include "document.h"

#include <QFutureWatcher>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace {
constexpr int Margin = 12;
constexpr int Spacing = 12;
constexpr double MinZoom = 0.1;
constexpr double MaxZoom = 8.0;
constexpr double ZoomStep = 1.2;
// Upper bound for one rendered page (~96 MB of ARGB). Beyond this the page is
// rendered smaller and upscaled, trading sharpness for bounded memory.
constexpr double MaxPixels = 24e6;
constexpr int PrerenderPages = 1; // rendered ahead on each side of the viewport
constexpr int KeepPages = 3; // cached on each side of the viewport
}

PdfView::PdfView(std::shared_ptr<Document> doc, QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_doc(std::move(doc))
{
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    // Always-on avoids a feedback loop between fit-width and the scroll bar
    // appearing/disappearing.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalScrollBar()->setSingleStep(48);
    horizontalScrollBar()->setSingleStep(48);
    viewport()->setCursor(Qt::OpenHandCursor);

    m_renderTimer.setSingleShot(true);
    connect(&m_renderTimer, &QTimer::timeout, this, &PdfView::startRenders);

    relayout();
}

PdfView::~PdfView()
{
    for (const Pending &p : std::as_const(m_pending)) {
        p.cancel->store(true);
    }
}

int PdfView::pageCount() const
{
    return m_doc->pageCount();
}

double PdfView::pxPerPoint() const
{
    return m_zoom * logicalDpiY() / 72.0;
}

void PdfView::relayout()
{
    const double s = pxPerPoint();
    const int n = m_doc->pageCount();
    m_rects.resize(n);

    int maxW = 0;
    for (int i = 0; i < n; ++i) {
        const QSizeF pt = m_doc->pageSize(i);
        const QSize px(std::max(1, qRound(pt.width() * s)), std::max(1, qRound(pt.height() * s)));
        m_rects[i] = QRect(QPoint(0, 0), px);
        maxW = std::max(maxW, px.width());
    }

    const int contentW = maxW + 2 * Margin;
    const int areaW = std::max(contentW, viewport()->width());
    int y = Margin;
    for (int i = 0; i < n; ++i) {
        QRect &r = m_rects[i];
        r.moveTo((areaW - r.width()) / 2, y);
        y += r.height() + Spacing;
    }
    m_contentSize = QSize(contentW, y - Spacing + Margin);
    updateScrollBars();
}

void PdfView::updateScrollBars()
{
    const QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, m_contentSize.width() - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, std::max(0, m_contentSize.height() - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
}

int PdfView::pageAtY(int y) const
{
    const auto it = std::lower_bound(m_rects.cbegin(), m_rects.cend(), y, [](const QRect &r, int value) {
        return r.bottom() < value;
    });
    if (it == m_rects.cend()) {
        return m_rects.size() - 1;
    }
    return int(it - m_rects.cbegin());
}

QPair<int, int> PdfView::visibleRange() const
{
    const int top = verticalScrollBar()->value();
    const int first = pageAtY(top);
    int last = pageAtY(top + viewport()->height());
    if (m_rects.at(last).top() > top + viewport()->height()) {
        last = std::max(first, last - 1);
    }
    return {first, last};
}

PdfView::Anchor PdfView::captureAnchor(QPoint viewportPos) const
{
    const QPoint c = viewportPos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    Anchor a;
    a.page = pageAtY(c.y());
    const QRect r = m_rects.at(a.page);
    a.fx = double(c.x() - r.x()) / r.width();
    a.fy = double(c.y() - r.y()) / r.height();
    return a;
}

void PdfView::restoreAnchor(const Anchor &anchor, QPoint viewportPos)
{
    const int page = std::clamp(anchor.page, 0, int(m_rects.size()) - 1);
    const QRect r = m_rects.at(page);
    const int cx = r.x() + qRound(anchor.fx * r.width());
    const int cy = r.y() + qRound(anchor.fy * r.height());
    horizontalScrollBar()->setValue(cx - viewportPos.x());
    verticalScrollBar()->setValue(cy - viewportPos.y());
}

void PdfView::applyZoom(double zoom, QPoint anchorPos)
{
    zoom = std::clamp(zoom, MinZoom, MaxZoom);
    if (!qFuzzyCompare(zoom, m_zoom)) {
        const Anchor a = captureAnchor(anchorPos);
        m_zoom = zoom;
        relayout();
        restoreAnchor(a, anchorPos);
    }
    updateCurrentPage();
    viewport()->update();
    requestRenders(80);
    Q_EMIT zoomChanged(m_zoom);
}

void PdfView::applyFit()
{
    if (m_mode == ZoomMode::Custom) {
        return;
    }
    const QSize vp = viewport()->size();
    if (vp.width() < 4 * Margin || vp.height() < 4 * Margin) {
        return;
    }
    double maxW = 1;
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        maxW = std::max(maxW, m_doc->pageSize(i).width());
    }
    const double unit = logicalDpiY() / 72.0; // pixels per point at 100 %
    double zoom = (vp.width() - 2 * Margin) / (maxW * unit);
    if (m_mode == ZoomMode::FitPage) {
        const double h = m_doc->pageSize(m_currentPage).height();
        zoom = std::min(zoom, (vp.height() - 2 * Margin) / (h * unit));
    }
    applyZoom(zoom, QPoint(vp.width() / 2, 0));
}

void PdfView::setZoom(double zoom)
{
    m_mode = ZoomMode::Custom;
    applyZoom(zoom, viewport()->rect().center());
}

void PdfView::setZoomMode(ZoomMode mode)
{
    m_mode = mode;
    applyFit();
    Q_EMIT zoomChanged(m_zoom);
}

void PdfView::zoomIn()
{
    setZoom(m_zoom * ZoomStep);
}

void PdfView::zoomOut()
{
    setZoom(m_zoom / ZoomStep);
}

void PdfView::goToPage(int page)
{
    page = std::clamp(page, 0, pageCount() - 1);
    verticalScrollBar()->setValue(m_rects.at(page).top() - Margin / 2);
}

PdfView::ViewState PdfView::viewState() const
{
    const Anchor a = captureAnchor(QPoint(0, 0));
    return {m_mode, m_zoom, a.page, a.fx, a.fy};
}

void PdfView::restoreViewState(const ViewState &state)
{
    m_mode = state.mode;
    m_zoom = std::clamp(state.zoom, MinZoom, MaxZoom);
    relayout();
    applyFit();
    restoreAnchor({state.page, state.fx, state.fy}, QPoint(0, 0));
    updateCurrentPage();
    viewport()->update();
    requestRenders(0);
    Q_EMIT zoomChanged(m_zoom);
}

void PdfView::updateCurrentPage()
{
    const int page = pageAtY(verticalScrollBar()->value() + viewport()->height() / 2);
    if (page != m_currentPage) {
        m_currentPage = page;
        Q_EMIT currentPageChanged(page);
    }
}

// --- rendering -------------------------------------------------------------

QSize PdfView::renderSize(int page) const
{
    const qreal dpr = devicePixelRatioF();
    const QSize logical = m_rects.at(page).size();
    double w = logical.width() * dpr;
    double h = logical.height() * dpr;
    if (w * h > MaxPixels) {
        const double f = std::sqrt(MaxPixels / (w * h));
        w *= f;
        h *= f;
    }
    return QSize(std::max(1, qRound(w)), std::max(1, qRound(h)));
}

void PdfView::requestRenders(int delayMs)
{
    // A longer delay already pending (zooming) must not be shortened by a
    // scroll event, or every wheel step would start a throw-away render.
    if (m_renderTimer.isActive() && m_renderTimer.remainingTime() >= delayMs) {
        return;
    }
    m_renderTimer.start(delayMs);
}

void PdfView::startRenders()
{
    if (!isVisible() || m_rects.isEmpty()) {
        return;
    }
    const auto [first, last] = visibleRange();
    const int n = m_rects.size();
    const int wantFirst = std::max(0, first - PrerenderPages);
    const int wantLast = std::min(n - 1, last + PrerenderPages);
    const int keepFirst = first - KeepPages;
    const int keepLast = last + KeepPages;

    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const int page = it.key();
        if (page < wantFirst || page > wantLast || it->size != renderSize(page)) {
            it->cancel->store(true);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    m_cache.removeIf([=](const std::pair<const int &, QImage &> entry) {
        return entry.first < keepFirst || entry.first > keepLast;
    });

    // Visible pages first, then the look-ahead ones.
    QList<int> order;
    for (int p = first; p <= last; ++p) {
        order.append(p);
    }
    for (int p = wantFirst; p <= wantLast; ++p) {
        if (p < first || p > last) {
            order.append(p);
        }
    }

    for (const int page : std::as_const(order)) {
        const QSize size = renderSize(page);
        const auto cached = m_cache.constFind(page);
        if ((cached != m_cache.constEnd() && cached->size() == size) || m_pending.contains(page)) {
            continue;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        m_pending.insert(page, {size, cancel});

        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, page, cancel] {
            watcher->deleteLater();
            const auto it = m_pending.constFind(page);
            if (it != m_pending.constEnd() && it->cancel == cancel) {
                m_pending.erase(it);
            }
            const QImage image = watcher->result();
            if (!cancel->load() && !image.isNull()) {
                m_cache.insert(page, image);
                viewport()->update();
            }
        });
        watcher->setFuture(QtConcurrent::run([doc = m_doc, page, size, cancel] {
            return cancel->load() ? QImage() : doc->render(page, size);
        }));
    }
}

void PdfView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    p.fillRect(event->rect(), palette().color(QPalette::Mid));
    if (m_rects.isEmpty()) {
        return;
    }
    p.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    const qreal dpr = devicePixelRatioF();
    const auto [first, last] = visibleRange();
    for (int page = first; page <= last; ++page) {
        const QRect r = m_rects.at(page);
        p.fillRect(r, Qt::white);
        const auto it = m_cache.constFind(page);
        if (it != m_cache.constEnd()) {
            const bool exact = it->size() == (QSizeF(r.size()) * dpr).toSize();
            p.setRenderHint(QPainter::SmoothPixmapTransform, !exact);
            p.drawImage(r, *it);
        }
        p.setPen(QColor(0, 0, 0, 60));
        p.drawRect(r.adjusted(-1, -1, 0, 0));
    }
}

// --- events ----------------------------------------------------------------

void PdfView::resizeEvent(QResizeEvent *)
{
    if (m_mode == ZoomMode::Custom) {
        const Anchor a = captureAnchor(QPoint(0, 0));
        relayout();
        restoreAnchor(a, QPoint(0, 0));
    } else {
        relayout();
        applyFit();
    }
    updateCurrentPage();
    requestRenders(0);
}

void PdfView::scrollContentsBy(int, int)
{
    viewport()->update();
    updateCurrentPage();
    requestRenders(0);
}

void PdfView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double steps = event->angleDelta().y() / 120.0;
        if (steps != 0) {
            m_mode = ZoomMode::Custom;
            applyZoom(m_zoom * std::pow(ZoomStep, steps), event->position().toPoint());
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PdfView::keyPressEvent(QKeyEvent *event)
{
    QScrollBar *v = verticalScrollBar();
    switch (event->key()) {
    case Qt::Key_Space:
        v->triggerAction(event->modifiers() & Qt::ShiftModifier ? QAbstractSlider::SliderPageStepSub
                                                                : QAbstractSlider::SliderPageStepAdd);
        return;
    case Qt::Key_Backspace:
        v->triggerAction(QAbstractSlider::SliderPageStepSub);
        return;
    case Qt::Key_Home:
        v->setValue(v->minimum());
        return;
    case Qt::Key_End:
        v->setValue(v->maximum());
        return;
    default:
        QAbstractScrollArea::keyPressEvent(event);
    }
}

void PdfView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_panning = true;
        m_panLast = event->globalPosition().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
    }
}

void PdfView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_panning) {
        return;
    }
    const QPoint now = event->globalPosition().toPoint();
    const QPoint delta = now - m_panLast;
    m_panLast = now;
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
}

void PdfView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_panning) {
        m_panning = false;
        viewport()->setCursor(Qt::OpenHandCursor);
    }
}

bool PdfView::event(QEvent *event)
{
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Show) {
        requestRenders(0);
    }
    return QAbstractScrollArea::event(event);
}
