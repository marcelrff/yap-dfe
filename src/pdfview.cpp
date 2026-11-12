#include "pdfview.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QStyleHints>
#include <QToolTip>
#include <QVBoxLayout>
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
constexpr double NoteSize = 20; // side of a comment icon, in points
constexpr double TextSize = 12; // font size of typed text, in points
constexpr int EditorInset = 3; // border + document margin of the inline editor

bool defaultCommentEditor(QWidget *parent, QString *text)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(PdfView::tr("Comment"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *edit = new QPlainTextEdit(*text, &dialog);
    edit->setTabChangesFocus(true);
    edit->moveCursor(QTextCursor::End);
    layout->addWidget(edit);
    auto *hint = new QLabel(PdfView::tr("Ctrl+Enter to save, Esc to cancel"), &dialog);
    hint->setEnabled(false);
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *accept = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), &dialog);
    QObject::connect(accept, &QShortcut::activated, &dialog, &QDialog::accept);
    dialog.resize(380, 220);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    *text = edit->toPlainText().trimmed();
    return true;
}

QFont editorFont(double pixelSize)
{
    QFont font(Document::textFontFamily());
    font.setPixelSize(std::max(1, qRound(pixelSize)));
    return font;
}

// Size in points of the box needed for `text` at the given font size.
QSizeF textBoxSize(const QString &text, double fontSize)
{
    // Measure at 10x for sub-pixel precision; a pixel then is a tenth of a point.
    const QFontMetricsF fm(editorFont(fontSize * 10));
    const QStringList lines = text.split(QLatin1Char('\n'));
    double width = 0;
    for (const QString &line : lines) {
        width = std::max(width, fm.horizontalAdvance(line));
    }
    // Poppler wraps text at the box edge, so leave some slack.
    return QSizeF(width / 10 * 1.05 + 4, lines.size() * fm.lineSpacing() / 10 + 4);
}
}

std::function<bool(QWidget *, QString *)> PdfView::commentEditor = defaultCommentEditor;

// Text box shown on the page while typing a Text annotation. Enter commits,
// Shift+Enter starts a new line, Esc cancels; losing focus commits too.
class InlineEditor : public QPlainTextEdit
{
public:
    InlineEditor(QWidget *parent, const QFont &font, std::function<void(bool)> finished)
        : QPlainTextEdit(parent)
        , m_finished(std::move(finished))
    {
        setFont(font);
        setFrameShape(QFrame::NoFrame);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        document()->setDocumentMargin(EditorInset - 1);
        setStyleSheet(QStringLiteral("QPlainTextEdit { background: rgba(255,255,255,235); color: black; border: 1px dashed #3daee9; }"));
        connect(this, &QPlainTextEdit::textChanged, this, &InlineEditor::fit);
        fit();
    }

    void fit()
    {
        const QFontMetrics fm(font());
        const QStringList lines = toPlainText().split(QLatin1Char('\n'));
        int width = 0;
        for (const QString &line : lines) {
            width = std::max(width, fm.horizontalAdvance(line));
        }
        setFixedSize(width + 2 * EditorInset + 2 * fm.averageCharWidth() + 4, int(lines.size()) * fm.lineSpacing() + 2 * EditorInset + 2);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_finished(false);
        } else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !(event->modifiers() & Qt::ShiftModifier)) {
            m_finished(true);
        } else {
            QPlainTextEdit::keyPressEvent(event);
        }
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QPlainTextEdit::focusOutEvent(event);
        // A context menu or another window taking focus is not "done typing".
        if (event->reason() != Qt::PopupFocusReason && event->reason() != Qt::ActiveWindowFocusReason) {
            m_finished(true);
        }
    }

private:
    std::function<void(bool)> m_finished;
};

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
    viewport()->setMouseTracking(true);

    m_renderTimer.setSingleShot(true);
    connect(&m_renderTimer, &QTimer::timeout, this, &PdfView::startRenders);
    connect(m_doc.get(), &Document::pageChanged, this, &PdfView::onPageChanged);

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
        finishEditor(true);
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
    m_stale.removeIf([this](int page) {
        return !m_cache.contains(page);
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
        const bool fresh = cached != m_cache.constEnd() && cached->size() == size && !m_stale.contains(page);
        if (fresh || m_pending.contains(page)) {
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
                m_stale.remove(page);
                m_overlays.remove(page); // the image now shows them itself
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
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(-1, -1, 0, 0));

        for (const Overlay &o : m_overlays.value(page)) {
            paintMarkup(p, page, o.kind, o.color, o.rects);
        }
    }

    // Text selection: in a markup tool it previews the annotation being made.
    if (hasTextSelection()) {
        const auto text = m_doc->cachedPageText(m_selPage);
        const auto [from, to] = selectionRange();
        if (text && to < text->size()) {
            const QList<QRectF> rects = lineRects(*text, from, to);
            if (m_tool == Tool::Highlight) {
                paintMarkup(p, m_selPage, AnnotationData::Kind::Highlight, m_toolColor, rects);
            } else if (m_tool == Tool::Underline || m_tool == Tool::StrikeOut) {
                paintMarkup(p, m_selPage, m_tool == Tool::Underline ? AnnotationData::Kind::Underline : AnnotationData::Kind::StrikeOut,
                            m_toolColor, rects);
            } else {
                QColor c = palette().color(QPalette::Highlight);
                c.setAlpha(90);
                for (const QRectF &r : rects) {
                    p.fillRect(toContent(m_selPage, r), c);
                }
            }
        }
    }

    // Outline of the selected annotation, following the mouse while it is moved.
    if (const std::optional<AnnotationData> a = selectedAnnotationData()) {
        QRectF r = toContent(a->page, a->boundary).adjusted(-2, -2, 2, 2);
        if (m_drag == Drag::MoveAnnot) {
            r.translate(m_lastPos - m_pressPos);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(palette().color(QPalette::Highlight), 1.5, Qt::DashLine));
        p.drawRect(r);
    }
}

void PdfView::paintMarkup(QPainter &p, int page, AnnotationData::Kind kind, const QColor &color, const QList<QRectF> &rects) const
{
    p.save();
    for (const QRectF &n : rects) {
        const QRectF r = toContent(page, n);
        if (kind == AnnotationData::Kind::Highlight) {
            p.setCompositionMode(QPainter::CompositionMode_Multiply);
            p.fillRect(r, color);
        } else {
            const double y = kind == AnnotationData::Kind::StrikeOut ? r.center().y() : r.bottom() - r.height() * 0.08;
            p.setPen(QPen(color, std::max(1.0, r.height() * 0.06)));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
    }
    p.restore();
}

void PdfView::onPageChanged(int page)
{
    // Whatever is cached or on its way predates the change.
    m_stale.insert(page);
    const auto it = m_pending.constFind(page);
    if (it != m_pending.constEnd()) {
        it->cancel->store(true);
        m_pending.erase(it);
    }
    viewport()->update();
    requestRenders(0);
}

// --- geometry helpers ------------------------------------------------------

bool PdfView::locate(QPoint viewportPos, int *page, QPointF *normalised) const
{
    if (m_rects.isEmpty()) {
        return false;
    }
    const QPoint c = viewportPos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const int index = pageAtY(c.y());
    if (!m_rects.at(index).contains(c)) {
        return false;
    }
    *page = index;
    *normalised = normalisedIn(index, viewportPos);
    return true;
}

QPointF PdfView::normalisedIn(int page, QPoint viewportPos) const
{
    const QPoint c = viewportPos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    const QRect r = m_rects.at(page);
    return QPointF(double(c.x() - r.x()) / r.width(), double(c.y() - r.y()) / r.height());
}

QRectF PdfView::toContent(int page, const QRectF &n) const
{
    const QRect r = m_rects.at(page);
    return QRectF(r.x() + n.x() * r.width(), r.y() + n.y() * r.height(), n.width() * r.width(), n.height() * r.height());
}

QPoint PdfView::viewportPoint(int page, QPointF n) const
{
    const QRect r = m_rects.at(page);
    return QPoint(r.x() + qRound(n.x() * r.width()), r.y() + qRound(n.y() * r.height()))
        - QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

// --- text selection --------------------------------------------------------

// Index of the character at a point. Exact mode only accepts points on or
// right next to a character (used to decide whether a drag selects text);
// nearest mode always answers if the page has text (used while dragging).
int PdfView::charAt(const PageText &text, int page, QPointF n, bool nearest) const
{
    const QSizeF pt = m_doc->pageSize(page);
    const QPointF p(n.x() * pt.width(), n.y() * pt.height());
    int best = -1;
    double bestScore = std::numeric_limits<double>::max();
    for (int i = 0; i < text.size(); ++i) {
        const QRectF &nr = text.at(i).rect;
        const QRectF r(nr.x() * pt.width(), nr.y() * pt.height(), nr.width() * pt.width(), nr.height() * pt.height());
        const double dx = p.x() < r.left() ? r.left() - p.x() : (p.x() > r.right() ? p.x() - r.right() : 0);
        const double dy = p.y() < r.top() ? r.top() - p.y() : (p.y() > r.bottom() ? p.y() - r.bottom() : 0);
        if (!nearest && (dy > 0 || dx > r.height() * 0.4)) {
            continue;
        }
        // Staying on the same line matters more than horizontal distance.
        const double score = dx + 6 * dy;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

QList<QRectF> PdfView::lineRects(const PageText &text, int from, int to)
{
    QList<QRectF> rects;
    QRectF current;
    for (int i = from; i <= to; ++i) {
        const TextChar &c = text.at(i);
        if (current.isNull()) {
            current = c.rect;
        } else if (c.rect.center().y() > current.top() && c.rect.center().y() < current.bottom()) {
            current |= c.rect;
        } else {
            rects.append(current);
            current = c.rect;
        }
        if (c.breakAfter) {
            rects.append(current);
            current = QRectF();
        }
    }
    if (!current.isNull()) {
        rects.append(current);
    }
    return rects;
}

bool PdfView::hasTextSelection() const
{
    return m_selPage >= 0 && m_selAnchor >= 0 && m_selCursor >= 0;
}

QPair<int, int> PdfView::selectionRange() const
{
    return {std::min(m_selAnchor, m_selCursor), std::max(m_selAnchor, m_selCursor)};
}

QString PdfView::selectedText() const
{
    if (!hasTextSelection()) {
        return {};
    }
    const auto text = m_doc->pageText(m_selPage);
    const auto [from, to] = selectionRange();
    QString result;
    for (int i = from; i <= to && i < text->size(); ++i) {
        const TextChar &c = text->at(i);
        result += c.text;
        if (i < to) {
            if (c.breakAfter) {
                result += QLatin1Char('\n');
            } else if (c.spaceAfter) {
                result += QLatin1Char(' ');
            }
        }
    }
    return result;
}

void PdfView::clearTextSelection()
{
    if (m_selPage >= 0) {
        m_selPage = m_selAnchor = m_selCursor = -1;
        viewport()->update();
        Q_EMIT selectionChanged();
    }
}

void PdfView::copySelection() const
{
    if (hasTextSelection()) {
        QGuiApplication::clipboard()->setText(selectedText());
    }
}

bool PdfView::markupSelection(AnnotationData::Kind kind, const QColor &color)
{
    if (!hasTextSelection()) {
        return false;
    }
    const auto text = m_doc->pageText(m_selPage);
    const auto [from, to] = selectionRange();
    if (to >= text->size()) {
        return false;
    }
    AnnotationData d;
    d.page = m_selPage;
    d.kind = kind;
    d.color = color;
    d.rects = lineRects(*text, from, to);
    for (const QRectF &r : std::as_const(d.rects)) {
        d.boundary |= r;
    }
    // Show it right away; the re-rendered page replaces this overlay.
    m_overlays[d.page].append({kind, color, d.rects});
    clearTextSelection();
    m_doc->addAnnotation(d);
    return true;
}

// --- annotations -----------------------------------------------------------

QList<QPair<QString, QColor>> PdfView::highlightColors()
{
    return {
        {tr("Yellow"), QColor(255, 233, 77)},
        {tr("Green"), QColor(142, 227, 142)},
        {tr("Blue"), QColor(143, 203, 255)},
        {tr("Pink"), QColor(255, 166, 201)},
    };
}

QColor PdfView::lineColor()
{
    return QColor(229, 57, 53);
}

QColor PdfView::noteColor()
{
    return QColor(255, 214, 0);
}

void PdfView::setTool(Tool tool, const QColor &color)
{
    finishEditor(true);
    m_tool = tool;
    m_toolColor = color;
    viewport()->update();
    updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
}

std::optional<AnnotationData> PdfView::annotationAt(int page, QPointF n, bool blocking) const
{
    QList<AnnotationData> list;
    if (blocking) {
        list = m_doc->annotations(page);
    } else if (!m_doc->cachedAnnotations(page, &list)) {
        return std::nullopt;
    }
    // Later annotations are drawn on top; icons and text win over markup.
    std::optional<AnnotationData> markup;
    for (auto it = list.crbegin(); it != list.crend(); ++it) {
        if (!it->isMarkup()) {
            if (it->boundary.contains(n)) {
                return *it;
            }
        } else if (!markup) {
            for (const QRectF &r : it->rects) {
                if (r.contains(n)) {
                    markup = *it;
                    break;
                }
            }
        }
    }
    return markup;
}

std::optional<AnnotationData> PdfView::selectedAnnotationData() const
{
    if (m_selAnnotId.isEmpty()) {
        return std::nullopt;
    }
    QList<AnnotationData> list;
    m_doc->cachedAnnotations(m_selAnnotPage, &list);
    for (const AnnotationData &d : std::as_const(list)) {
        if (d.id == m_selAnnotId) {
            return d;
        }
    }
    return std::nullopt;
}

void PdfView::selectAnnotation(int page, const QString &id)
{
    if (m_selAnnotId != id || m_selAnnotPage != page) {
        m_selAnnotPage = page;
        m_selAnnotId = id;
        viewport()->update();
        Q_EMIT selectionChanged();
    }
}

bool PdfView::deleteSelectedAnnotation()
{
    if (!selectedAnnotationData()) {
        return false;
    }
    const QString id = m_selAnnotId;
    const int page = m_selAnnotPage;
    selectAnnotation(-1, QString());
    m_doc->removeAnnotation(page, id);
    return true;
}

void PdfView::editAnnotation(const AnnotationData &annotation)
{
    if (annotation.kind == AnnotationData::Kind::Text) {
        openEditor(annotation.page, annotation.boundary.topLeft(), annotation);
        return;
    }
    AnnotationData edited = annotation;
    if (commentEditor(this, &edited.contents)) {
        m_doc->updateAnnotation(edited);
    }
}

void PdfView::createNote(int page, QPointF n)
{
    QString text;
    if (!commentEditor(this, &text) || text.trimmed().isEmpty()) {
        return;
    }
    const QSizeF pt = m_doc->pageSize(page);
    const QSizeF size(NoteSize / pt.width(), NoteSize / pt.height());
    AnnotationData d;
    d.page = page;
    d.kind = AnnotationData::Kind::Note;
    d.color = noteColor();
    d.contents = text.trimmed();
    d.boundary = QRectF(std::clamp(n.x() - size.width() / 2, 0.0, 1.0 - size.width()),
                        std::clamp(n.y() - size.height() / 2, 0.0, 1.0 - size.height()), size.width(), size.height());
    const QString id = m_doc->addAnnotation(d);
    selectAnnotation(page, id);
    Q_EMIT toolFinished();
}

void PdfView::openEditor(int page, QPointF topLeft, const std::optional<AnnotationData> &existing)
{
    finishEditor(true);
    const double fontSize = existing ? existing->fontSize : TextSize;
    m_editPage = page;
    m_editExisting = existing;
    m_editor = new InlineEditor(viewport(), editorFont(fontSize * pxPerPoint()), [this](bool accept) {
        finishEditor(accept);
    });
    if (existing) {
        m_editor->setPlainText(existing->contents);
        m_editor->moveCursor(QTextCursor::End);
    }
    m_editor->move(viewportPoint(page, topLeft) - QPoint(EditorInset, EditorInset));
    m_editor->show();
    m_editor->setFocus();
}

void PdfView::finishEditor(bool accept)
{
    if (!m_editor) {
        return;
    }
    InlineEditor *editor = m_editor;
    m_editor = nullptr; // hiding the editor below re-enters through its focus-out
    const std::optional<AnnotationData> existing = m_editExisting;
    m_editExisting.reset();

    QString text = editor->toPlainText();
    while (!text.isEmpty() && text.back().isSpace()) {
        text.chop(1);
    }
    const QPointF topLeft = normalisedIn(m_editPage, editor->pos() + QPoint(EditorInset, EditorInset));
    const bool hadFocus = editor->hasFocus();
    editor->hide();
    editor->deleteLater();
    if (hadFocus) {
        setFocus();
    }
    if (!accept) {
        return;
    }

    const double fontSize = existing ? existing->fontSize : TextSize;
    const QSizeF pt = m_doc->pageSize(m_editPage);
    const QSizeF box = textBoxSize(text, fontSize);
    const QSizeF size(box.width() / pt.width(), box.height() / pt.height());
    if (existing) {
        if (text.isEmpty()) {
            m_doc->removeAnnotation(existing->page, existing->id);
        } else {
            AnnotationData d = *existing;
            d.contents = text;
            d.boundary = QRectF(existing->boundary.topLeft(), size);
            m_doc->updateAnnotation(d);
        }
    } else if (!text.isEmpty()) {
        AnnotationData d;
        d.page = m_editPage;
        d.kind = AnnotationData::Kind::Text;
        d.color = Qt::black;
        d.fontSize = fontSize;
        d.contents = text;
        d.boundary = QRectF(topLeft, size);
        m_doc->addAnnotation(d);
    }
}

// --- events ----------------------------------------------------------------

void PdfView::resizeEvent(QResizeEvent *)
{
    finishEditor(true);
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
    finishEditor(true);
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
    case Qt::Key_Escape:
        // Esc backs out one step at a time: selection first, then the tool.
        if (hasTextSelection() || !m_selAnnotId.isEmpty()) {
            clearTextSelection();
            selectAnnotation(-1, QString());
        } else {
            Q_EMIT toolFinished();
        }
        return;
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

void PdfView::startPan(QMouseEvent *event)
{
    m_drag = Drag::Pan;
    m_panLast = event->globalPosition().toPoint();
    viewport()->setCursor(Qt::ClosedHandCursor);
}

void PdfView::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    m_pressPos = m_lastPos = pos;
    if (event->button() == Qt::MiddleButton) {
        startPan(event);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    int page;
    QPointF n;
    if (!locate(pos, &page, &n)) {
        clearTextSelection();
        selectAnnotation(-1, QString());
        startPan(event);
        return;
    }

    switch (m_tool) {
    case Tool::Select: {
        if (const std::optional<AnnotationData> a = annotationAt(page, n, true)) {
            // A click selects it; what a drag does is decided once it starts.
            m_pressAnnot = a;
            m_drag = Drag::PendingAnnot;
            return;
        }
        selectAnnotation(-1, QString());
        clearTextSelection();
        const int c = charAt(*m_doc->pageText(page), page, n, false);
        if (c >= 0) {
            m_selPage = page;
            m_selAnchor = c;
            m_drag = Drag::SelectText;
        } else {
            startPan(event);
        }
        break;
    }
    case Tool::Highlight:
    case Tool::Underline:
    case Tool::StrikeOut: {
        selectAnnotation(-1, QString());
        clearTextSelection();
        const int c = charAt(*m_doc->pageText(page), page, n, true);
        if (c >= 0) {
            m_selPage = page;
            m_selAnchor = c;
            m_drag = Drag::SelectText;
        }
        break;
    }
    case Tool::Note:
        createNote(page, n);
        break;
    case Tool::Text: {
        const std::optional<AnnotationData> a = annotationAt(page, n, true);
        if (a && a->kind == AnnotationData::Kind::Text) {
            editAnnotation(*a);
        } else {
            // The click marks the middle of the first line.
            const double half = TextSize * 0.7 / m_doc->pageSize(page).height();
            openEditor(page, QPointF(n.x(), n.y() - half), std::nullopt);
        }
        break;
    }
    }
}

void PdfView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    m_lastPos = pos;
    switch (m_drag) {
    case Drag::None:
        updateCursor(pos);
        break;
    case Drag::Pan: {
        const QPoint now = event->globalPosition().toPoint();
        const QPoint delta = now - m_panLast;
        m_panLast = now;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        break;
    }
    case Drag::PendingAnnot:
        if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            break;
        }
        if (!m_pressAnnot->isMarkup()) {
            selectAnnotation(m_pressAnnot->page, m_pressAnnot->id);
            clearTextSelection();
            m_drag = Drag::MoveAnnot;
            viewport()->update();
            break;
        }
        // Dragging across highlighted text selects the text underneath.
        selectAnnotation(-1, QString());
        m_selPage = m_pressAnnot->page;
        m_selAnchor = charAt(*m_doc->pageText(m_selPage), m_selPage, normalisedIn(m_selPage, m_pressPos), true);
        m_drag = Drag::SelectText;
        [[fallthrough]];
    case Drag::SelectText: {
        const int c = charAt(*m_doc->pageText(m_selPage), m_selPage, normalisedIn(m_selPage, pos), true);
        if (c != m_selCursor) {
            m_selCursor = c;
            viewport()->update();
        }
        break;
    }
    case Drag::MoveAnnot:
        viewport()->update();
        break;
    }
}

void PdfView::mouseReleaseEvent(QMouseEvent *event)
{
    const Drag drag = m_drag;
    if ((drag == Drag::Pan && event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton)
        || (drag != Drag::Pan && event->button() != Qt::LeftButton)) {
        return;
    }
    m_drag = Drag::None;
    const QPoint pos = event->position().toPoint();

    switch (drag) {
    case Drag::SelectText:
        if (!hasTextSelection()) {
            m_selPage = m_selAnchor = m_selCursor = -1;
        } else if (m_tool == Tool::Highlight) {
            markupSelection(AnnotationData::Kind::Highlight, m_toolColor);
        } else if (m_tool == Tool::Underline) {
            markupSelection(AnnotationData::Kind::Underline, m_toolColor);
        } else if (m_tool == Tool::StrikeOut) {
            markupSelection(AnnotationData::Kind::StrikeOut, m_toolColor);
        } else {
            Q_EMIT selectionChanged();
        }
        break;
    case Drag::PendingAnnot:
        clearTextSelection();
        selectAnnotation(m_pressAnnot->page, m_pressAnnot->id);
        break;
    case Drag::MoveAnnot:
        if (const std::optional<AnnotationData> a = selectedAnnotationData()) {
            const QRect r = m_rects.at(a->page);
            const QPoint delta = pos - m_pressPos;
            AnnotationData moved = *a;
            moved.boundary.translate(double(delta.x()) / r.width(), double(delta.y()) / r.height());
            // Keep at least the top-left corner on the page.
            moved.boundary.moveTo(std::clamp(moved.boundary.x(), 0.0, 0.98), std::clamp(moved.boundary.y(), 0.0, 0.98));
            m_doc->updateAnnotation(moved);
        }
        break;
    default:
        break;
    }
    m_pressAnnot.reset();
    viewport()->update();
    updateCursor(pos);
}

void PdfView::mouseDoubleClickEvent(QMouseEvent *event)
{
    int page;
    QPointF n;
    if (event->button() != Qt::LeftButton || m_tool != Tool::Select || !locate(event->position().toPoint(), &page, &n)) {
        return;
    }
    if (const std::optional<AnnotationData> a = annotationAt(page, n, true)) {
        m_drag = Drag::None;
        selectAnnotation(a->page, a->id);
        editAnnotation(*a);
        return;
    }
    // Select the word under the pointer.
    const auto text = m_doc->pageText(page);
    const int c = charAt(*text, page, n, false);
    if (c < 0) {
        return;
    }
    int from = c, to = c;
    while (from > 0 && text->at(from - 1).word == text->at(c).word) {
        --from;
    }
    while (to + 1 < text->size() && text->at(to + 1).word == text->at(c).word) {
        ++to;
    }
    m_drag = Drag::None;
    m_selPage = page;
    m_selAnchor = from;
    m_selCursor = to;
    viewport()->update();
    Q_EMIT selectionChanged();
}

void PdfView::contextMenuEvent(QContextMenuEvent *event)
{
    const QPoint pos = viewport()->mapFromGlobal(event->globalPos());
    int page;
    QPointF n;
    if (!locate(pos, &page, &n)) {
        return;
    }
    QMenu menu(this);
    if (const std::optional<AnnotationData> a = annotationAt(page, n, true)) {
        selectAnnotation(a->page, a->id);
        menu.addAction(a->kind == AnnotationData::Kind::Text ? tr("Edit Text") : tr("Edit Comment…"), this, [this, a] {
            editAnnotation(*a);
        });
        if (a->kind == AnnotationData::Kind::Highlight) {
            QMenu *colors = menu.addMenu(tr("Colour"));
            for (const auto &[name, color] : highlightColors()) {
                colors->addAction(name, this, [this, a, color = color] {
                    AnnotationData d = *a;
                    d.color = color;
                    m_doc->updateAnnotation(d);
                });
            }
        }
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this] {
            deleteSelectedAnnotation();
        });
    } else if (hasTextSelection()) {
        menu.addAction(tr("Copy"), this, &PdfView::copySelection);
        menu.addSeparator();
        for (const auto &[name, color] : highlightColors()) {
            menu.addAction(tr("Highlight %1").arg(name), this, [this, color = color] {
                markupSelection(AnnotationData::Kind::Highlight, color);
            });
        }
        menu.addAction(tr("Underline"), this, [this] {
            markupSelection(AnnotationData::Kind::Underline, lineColor());
        });
        menu.addAction(tr("Strike Out"), this, [this] {
            markupSelection(AnnotationData::Kind::StrikeOut, lineColor());
        });
    } else {
        return;
    }
    menu.exec(event->globalPos());
}

void PdfView::updateCursor(QPoint pos)
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    QString tip;
    int page;
    QPointF n;
    if (locate(pos, &page, &n)) {
        // Hovering must never wait for a render, so only use what is cached.
        const std::optional<AnnotationData> a = annotationAt(page, n, false);
        const auto text = m_doc->cachedPageText(page);
        const bool onText = text && charAt(*text, page, n, false) >= 0;
        switch (m_tool) {
        case Tool::Select:
            if (a) {
                shape = Qt::PointingHandCursor;
            } else if (onText) {
                shape = Qt::IBeamCursor;
            }
            break;
        case Tool::Highlight:
        case Tool::Underline:
        case Tool::StrikeOut:
            shape = Qt::IBeamCursor;
            break;
        case Tool::Note:
            shape = Qt::CrossCursor;
            break;
        case Tool::Text:
            shape = Qt::IBeamCursor;
            break;
        }
        if (a && a->kind != AnnotationData::Kind::Text) {
            tip = a->contents;
        }
    }
    viewport()->setCursor(shape);
    if (tip.isEmpty()) {
        QToolTip::hideText();
    } else {
        QToolTip::showText(viewport()->mapToGlobal(pos), tip, viewport());
    }
}

bool PdfView::event(QEvent *event)
{
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Show) {
        requestRenders(0);
    }
    return QAbstractScrollArea::event(event);
}
