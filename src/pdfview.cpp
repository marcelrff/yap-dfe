#include "pdfview.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QStyleHints>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <utility>

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
constexpr double PenWidth = 2; // width of a pen stroke, in points
constexpr int EditorInset = 3; // border + document margin of the inline editor
constexpr int MaxHistory = 100; // remembered jumps

constexpr int DarkFloor = 28; // darkest value on a dark page: dark grey, not black

// Inverts lightness but keeps hue and saturation, so a yellow highlight
// stays yellow. Adding the same amount to all three channels does exactly
// that; the result is then lifted so that white paper becomes dark grey.
inline QRgb darkenPixel(QRgb px)
{
    const int r = qRed(px), g = qGreen(px), b = qBlue(px);
    const int d = 255 - std::max({r, g, b}) - std::min({r, g, b});
    const auto lift = [](int v) {
        return DarkFloor + v * (255 - DarkFloor) / 255;
    };
    return qRgb(lift(r + d), lift(g + d), lift(b + d));
}

void darkenImage(QImage &image)
{
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_RGB32);
    }
    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            line[x] = darkenPixel(line[x]);
        }
    }
}

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

// Web and mail links open right away. Anything else (a local file, some
// other program's scheme) could run something, so the user confirms first.
void defaultUrlOpener(QWidget *parent, const QUrl &url)
{
    static const QStringList harmless{QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("mailto")};
    if (!harmless.contains(url.scheme().toLower())) {
        const QString shown = url.toDisplayString();
        if (QMessageBox::question(parent, PdfView::tr("Open link"), PdfView::tr("The document links to\n\n%1\n\nOpen it?").arg(shown))
            != QMessageBox::Yes) {
            return;
        }
    }
    QDesktopServices::openUrl(url);
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
std::function<void(QWidget *, const QUrl &)> PdfView::urlOpener = defaultUrlOpener;

// Text box shown on the page while typing a Text annotation. Enter commits,
// Shift+Enter starts a new line, Esc cancels; losing focus commits too.
class InlineEditor : public QPlainTextEdit
{
public:
    InlineEditor(QWidget *parent, const QFont &font, bool dark, std::function<void(bool)> finished)
        : QPlainTextEdit(parent)
        , m_finished(std::move(finished))
    {
        setFont(font);
        setFrameShape(QFrame::NoFrame);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        document()->setDocumentMargin(EditorInset - 1);
        setStyleSheet(dark ? QStringLiteral("QPlainTextEdit { background: rgba(28,28,28,235); color: white; border: 1px dashed #3daee9; }")
                           : QStringLiteral("QPlainTextEdit { background: rgba(255,255,255,235); color: black; border: 1px dashed #3daee9; }"));
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
    connect(m_doc.get(), &Document::pagesChanged, this, &PdfView::onPagesChanged);

    relayout();
}

PdfView::~PdfView()
{
    for (const Pending &p : std::as_const(m_pending)) {
        p.cancel->store(true);
    }
    cancelSearch();
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

// --- links and history ---------------------------------------------------------

void PdfView::restorePosition(const Anchor &anchor)
{
    restoreAnchor(anchor, QPoint(0, 0));
    updateCurrentPage();
}

void PdfView::scrollTo(int page, double left, double top)
{
    page = std::clamp(page, 0, pageCount() - 1);
    const QRect r = m_rects.at(page);
    const int y = top >= 0 ? r.top() + qRound(top * r.height()) : r.top();
    verticalScrollBar()->setValue(y - Margin / 2);
    if (left >= 0) {
        // Only scroll sideways if the target is out of view.
        QScrollBar *h = horizontalScrollBar();
        const int x = r.left() + qRound(left * r.width());
        if (x < h->value() || x > h->value() + viewport()->width() - Margin) {
            h->setValue(x - Margin / 2);
        }
    }
    updateCurrentPage();
}

void PdfView::jumpTo(int page, double left, double top)
{
    m_back.append(captureAnchor(QPoint(0, 0)));
    if (m_back.size() > MaxHistory) {
        m_back.removeFirst();
    }
    m_forward.clear();
    scrollTo(page, left, top);
    Q_EMIT historyChanged();
}

void PdfView::goBack()
{
    if (m_back.isEmpty()) {
        return;
    }
    m_forward.append(captureAnchor(QPoint(0, 0)));
    restorePosition(m_back.takeLast());
    Q_EMIT historyChanged();
}

void PdfView::goForward()
{
    if (m_forward.isEmpty()) {
        return;
    }
    m_back.append(captureAnchor(QPoint(0, 0)));
    restorePosition(m_forward.takeLast());
    Q_EMIT historyChanged();
}

std::optional<LinkData> PdfView::linkAt(int page, QPointF n, bool blocking) const
{
    QList<LinkData> list;
    if (blocking) {
        list = m_doc->links(page);
    } else if (!m_doc->cachedLinks(page, &list)) {
        return std::nullopt;
    }
    // The smallest area wins where links overlap.
    std::optional<LinkData> best;
    for (const LinkData &link : std::as_const(list)) {
        if (link.area.contains(n) && (!best || link.area.width() * link.area.height() < best->area.width() * best->area.height())) {
            best = link;
        }
    }
    return best;
}

void PdfView::activateLink(const LinkData &link)
{
    switch (link.kind) {
    case LinkData::Kind::Goto:
        jumpTo(link.page, link.left, link.top);
        break;
    case LinkData::Kind::Uri: {
        // Some PDFs leave out the scheme ("www.example.org").
        QUrl url(link.target);
        if (url.scheme().isEmpty()) {
            url = QUrl::fromUserInput(link.target);
        }
        if (url.isValid()) {
            urlOpener(this, url);
        }
        break;
    }
    case LinkData::Kind::File: {
        // Relative names are relative to this document.
        QString path = link.target;
        if (path.startsWith(QStringLiteral("file:"))) {
            path = QUrl(path).toLocalFile();
        }
        Q_EMIT openFileRequested(QFileInfo(QFileInfo(m_doc->path()).dir(), path).absoluteFilePath(), link.page);
        break;
    }
    case LinkData::Kind::Action:
        switch (link.action) {
        case LinkData::Action::FirstPage:
            jumpTo(0);
            break;
        case LinkData::Action::PreviousPage:
            jumpTo(m_currentPage - 1);
            break;
        case LinkData::Action::NextPage:
            jumpTo(m_currentPage + 1);
            break;
        case LinkData::Action::LastPage:
            jumpTo(pageCount() - 1);
            break;
        case LinkData::Action::Back:
            goBack();
            break;
        case LinkData::Action::Forward:
            goForward();
            break;
        case LinkData::Action::None:
            break;
        }
        break;
    }
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
        watcher->setFuture(QtConcurrent::run([doc = m_doc, page, size, cancel, dark = m_dark] {
            if (cancel->load()) {
                return QImage();
            }
            QImage image = doc->render(page, size);
            if (dark && !image.isNull()) {
                darkenImage(image);
            }
            return image;
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
        p.fillRect(r, m_dark ? darkened(Qt::white) : QColor(Qt::white));
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
            if (o.kind == AnnotationData::Kind::Ink) {
                paintStroke(p, page, o.color, o.stroke);
            } else {
                paintMarkup(p, page, o.kind, o.color, o.rects);
            }
        }
    }
    if (m_drag == Drag::Draw) {
        paintStroke(p, m_strokePage, m_toolColor, m_stroke);
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

    // Search matches; the current one stands out.
    for (int page = first; page <= last; ++page) {
        const auto hits = m_hits.constFind(page);
        if (hits == m_hits.constEnd()) {
            continue;
        }
        p.setPen(Qt::NoPen);
        for (int i = 0; i < hits->size(); ++i) {
            const bool current = page == m_hitPage && i == m_hitIndex;
            for (const QRectF &n : hits->at(i)) {
                const QRectF r = toContent(page, n).adjusted(-1, -1, 1, 1);
                p.fillRect(r, current ? QColor(255, 140, 0, 130) : QColor(255, 215, 0, 90));
                if (current) {
                    p.setPen(QPen(QColor(255, 120, 0), 1.5));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(r);
                    p.setPen(Qt::NoPen);
                }
            }
        }
    }

    // Outlines of the selected annotations; the movable ones follow the mouse
    // while they are dragged.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(palette().color(QPalette::Highlight), 1.5, Qt::DashLine));
    for (const AnnotationData &a : selectedAnnotations()) {
        if (a.isMarkup()) {
            for (const QRectF &n : a.rects) {
                p.drawRect(toContent(a.page, n).adjusted(-1, -1, 1, 1));
            }
            continue;
        }
        QRectF r = toContent(a.page, a.boundary).adjusted(-2, -2, 2, 2);
        if (m_drag == Drag::MoveAnnot) {
            r.translate(m_lastPos - m_pressPos);
        }
        p.drawRect(r);
    }

    if (m_drag == Drag::Band) {
        const QColor c = palette().color(QPalette::Highlight);
        QColor fill = c;
        fill.setAlpha(40);
        p.setPen(QPen(c, 1));
        p.setBrush(fill);
        p.drawRect(bandRect());
    }
}

void PdfView::paintMarkup(QPainter &p, int page, AnnotationData::Kind kind, const QColor &color, const QList<QRectF> &rects) const
{
    p.save();
    for (const QRectF &n : rects) {
        const QRectF r = toContent(page, n);
        if (kind == AnnotationData::Kind::Highlight && m_dark) {
            // What multiplying does on white paper, adding does on a dark page.
            const QColor c = darkened(color);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.fillRect(r, QColor(c.red() - DarkFloor, c.green() - DarkFloor, c.blue() - DarkFloor));
        } else if (kind == AnnotationData::Kind::Highlight) {
            p.setCompositionMode(QPainter::CompositionMode_Multiply);
            p.fillRect(r, color);
        } else {
            const double y = kind == AnnotationData::Kind::StrikeOut ? r.center().y() : r.bottom() - r.height() * 0.08;
            p.setPen(QPen(m_dark ? darkened(color) : color, std::max(1.0, r.height() * 0.06)));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
    }
    p.restore();
}

void PdfView::paintStroke(QPainter &p, int page, const QColor &color, const QList<QPointF> &stroke) const
{
    if (stroke.isEmpty()) {
        return;
    }
    const QRect r = m_rects.at(page);
    QPolygonF line;
    for (const QPointF &n : stroke) {
        line.append(QPointF(r.x() + n.x() * r.width(), r.y() + n.y() * r.height()));
    }
    if (line.size() == 1) {
        line.append(line.first()); // a dot
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_dark ? darkened(color) : color, std::max(1.0, PenWidth * pxPerPoint()), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(line);
    p.restore();
}

// Turns the stroke just drawn into an annotation of its own, so that each
// one can be undone, moved and deleted separately.
void PdfView::finishStroke()
{
    const QList<QPointF> stroke = std::exchange(m_stroke, {});
    const int page = std::exchange(m_strokePage, -1);
    if (page < 0 || stroke.isEmpty()) {
        return;
    }
    const QSizeF pt = m_doc->pageSize(page);
    AnnotationData d;
    d.page = page;
    d.kind = AnnotationData::Kind::Ink;
    d.color = m_toolColor;
    d.width = PenWidth;
    d.paths = {stroke};
    if (stroke.size() == 1) {
        // A click: a dot. A single point would not be drawn at all.
        d.paths.first().append(stroke.first() + QPointF(0.1 / pt.width(), 0));
    }
    const double mx = PenWidth / pt.width(), my = PenWidth / pt.height();
    d.boundary = QPolygonF(d.paths.first()).boundingRect().adjusted(-mx, -my, mx, my);
    // Show it right away; the re-rendered page replaces this overlay.
    m_overlays[page].append({d.kind, d.color, {}, stroke});
    m_doc->addAnnotation(d);
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

void PdfView::onPagesChanged()
{
    // Every page number may now mean another page: drop all that refers to one.
    for (const Pending &p : std::as_const(m_pending)) {
        p.cancel->store(true);
    }
    m_pending.clear();
    m_cache.clear();
    m_stale.clear();
    m_overlays.clear();
    if (m_editor) {
        finishEditor(false);
    }
    m_drag = Drag::None;
    m_pressAnnot.reset();
    m_pressLink.reset();
    clearTextSelection();
    selectAnnotation(-1, QString());
    m_back.clear();
    m_forward.clear();
    Q_EMIT historyChanged();

    const int page = std::min(m_currentPage, m_doc->pageCount() - 1);
    relayout();
    verticalScrollBar()->setValue(m_rects.at(page).top() - Margin);
    m_currentPage = -1;
    updateCurrentPage();

    // Search again, without jumping to the first match.
    const QString text = m_searchText;
    cancelSearch();
    m_searchText.clear();
    m_hits.clear();
    m_hitPage = m_hitIndex = -1;
    if (!text.isEmpty()) {
        setSearch(text, m_searchCase);
        m_hitPage = m_currentPage;
    }
    Q_EMIT searchChanged();
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

QColor PdfView::penColor()
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
        if (it->kind == AnnotationData::Kind::Ink) {
            // On or right next to the line itself, not anywhere in its box.
            const QSizeF pt = m_doc->pageSize(page);
            const auto points = [pt](QPointF q) {
                return QPointF(q.x() * pt.width(), q.y() * pt.height());
            };
            const double reach = std::max(4.0, it->width);
            const QPointF c = points(n);
            for (const QList<QPointF> &path : it->paths) {
                for (int i = 0; i + 1 < path.size(); ++i) {
                    const QPointF a = points(path.at(i)), ab = points(path.at(i + 1)) - a;
                    const double len2 = QPointF::dotProduct(ab, ab);
                    const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(c - a, ab) / len2, 0.0, 1.0) : 0.0;
                    const QPointF d = c - (a + t * ab);
                    if (QPointF::dotProduct(d, d) <= reach * reach) {
                        return *it;
                    }
                }
            }
        } else if (!it->isMarkup()) {
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

QList<AnnotationData> PdfView::selectedAnnotations() const
{
    QList<AnnotationData> result;
    QHash<int, QList<AnnotationData>> pages;
    for (const auto &[page, id] : m_selAnnots) {
        auto it = pages.find(page);
        if (it == pages.end()) {
            QList<AnnotationData> list;
            m_doc->cachedAnnotations(page, &list);
            it = pages.insert(page, list);
        }
        for (const AnnotationData &d : std::as_const(*it)) {
            if (d.id == id) {
                result.append(d);
                break;
            }
        }
    }
    return result;
}

QString PdfView::selectedAnnotation() const
{
    const QList<AnnotationData> list = selectedAnnotations();
    return list.isEmpty() ? QString() : list.first().id;
}

bool PdfView::isAnnotationSelected(const AnnotationData &a) const
{
    return m_selAnnots.contains(AnnotKey(a.page, a.id));
}

void PdfView::setAnnotationSelection(const QList<AnnotKey> &keys)
{
    if (m_selAnnots != keys) {
        m_selAnnots = keys;
        viewport()->update();
        Q_EMIT selectionChanged();
    }
}

void PdfView::selectAnnotation(int page, const QString &id)
{
    setAnnotationSelection(id.isEmpty() ? QList<AnnotKey>() : QList<AnnotKey>{AnnotKey(page, id)});
}

void PdfView::selectAllAnnotations()
{
    clearTextSelection();
    QList<AnnotKey> keys;
    const int n = pageCount();
    for (int page = 0; page < n; ++page) {
        for (const AnnotationData &d : m_doc->annotations(page)) {
            keys.append(AnnotKey(page, d.id));
        }
    }
    setAnnotationSelection(keys);
}

bool PdfView::deleteSelectedAnnotations()
{
    QList<AnnotKey> keys;
    for (const AnnotationData &d : selectedAnnotations()) {
        keys.append(AnnotKey(d.page, d.id));
    }
    if (keys.isEmpty()) {
        return false;
    }
    selectAnnotation(-1, QString());
    m_doc->removeAnnotations(keys);
    return true;
}

QRect PdfView::bandRect() const
{
    const QPoint end = m_lastPos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
    return QRect(m_bandOrigin, end).normalized();
}

QList<PdfView::AnnotKey> PdfView::annotationsIn(const QRect &content) const
{
    QList<AnnotKey> keys;
    for (int page = 0; page < m_rects.size(); ++page) {
        const QRect r = m_rects.at(page);
        if (!r.intersects(content)) {
            continue;
        }
        const QRect c = r.intersected(content);
        const QRectF n(double(c.x() - r.x()) / r.width(), double(c.y() - r.y()) / r.height(),
                       double(c.width()) / r.width(), double(c.height()) / r.height());
        for (const AnnotationData &d : m_doc->annotations(page)) {
            bool hit = false;
            if (d.isMarkup()) {
                for (const QRectF &q : d.rects) {
                    hit = hit || q.intersects(n);
                }
            } else if (d.kind == AnnotationData::Kind::Ink) {
                for (const QList<QPointF> &path : d.paths) {
                    for (const QPointF &point : path) {
                        hit = hit || n.contains(point);
                    }
                }
            } else {
                hit = d.boundary.intersects(n);
            }
            if (hit) {
                keys.append(AnnotKey(page, d.id));
            }
        }
    }
    return keys;
}

void PdfView::updateBand()
{
    QList<AnnotKey> keys = m_bandBase;
    for (const AnnotKey &k : annotationsIn(bandRect())) {
        if (!keys.contains(k)) {
            keys.append(k);
        }
    }
    setAnnotationSelection(keys);
    viewport()->update();
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
    m_editor = new InlineEditor(viewport(), editorFont(fontSize * pxPerPoint()), m_dark, [this](bool accept) {
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

// --- dark mode ---------------------------------------------------------------

QColor PdfView::darkened(const QColor &color)
{
    return QColor::fromRgb(darkenPixel(color.rgb()));
}

void PdfView::setDarkMode(bool dark)
{
    if (dark == m_dark) {
        return;
    }
    finishEditor(true);
    m_dark = dark;
    // Every cached image is in the other mode now.
    for (const Pending &p : std::as_const(m_pending)) {
        p.cancel->store(true);
    }
    m_pending.clear();
    m_cache.clear();
    m_stale.clear();
    viewport()->update();
    requestRenders(0);
}

// --- search ------------------------------------------------------------------

void PdfView::cancelSearch()
{
    if (m_searchWatcher) {
        m_searchWatcher->disconnect(this);
        m_searchWatcher->cancel();
        m_searchWatcher->deleteLater();
        m_searchWatcher = nullptr;
    }
}

void PdfView::setSearch(const QString &text, bool caseSensitive)
{
    if (text == m_searchText && caseSensitive == m_searchCase) {
        return;
    }
    cancelSearch();
    // Keep looking from where the current match was, so that typing more
    // letters refines the match instead of jumping away.
    const int start = m_hitPage >= 0 ? m_hitPage : m_currentPage;
    m_searchText = text;
    m_searchCase = caseSensitive;
    m_hits.clear();
    m_hitPage = m_hitIndex = -1;
    viewport()->update();

    if (!text.isEmpty()) {
        m_searchWatcher = new QFutureWatcher<PageHits>(this);
        connect(m_searchWatcher, &QFutureWatcher<PageHits>::resultReadyAt, this, [this, watcher = m_searchWatcher](int index) {
            const PageHits hits = watcher->resultAt(index);
            m_hits.insert(hits.page, hits.matches);
            if (m_hitPage < 0) {
                // Pages arrive starting at the current one, so the first
                // result is the nearest match ahead.
                m_hitPage = hits.page;
                m_hitIndex = 0;
                showCurrentMatch();
            }
            viewport()->update();
            Q_EMIT searchChanged();
        });
        connect(m_searchWatcher, &QFutureWatcher<PageHits>::finished, this, &PdfView::searchChanged);
        m_searchWatcher->setFuture(QtConcurrent::run([doc = m_doc, text, caseSensitive, start](QPromise<PageHits> &promise) {
            const int n = doc->pageCount();
            for (int i = 0; i < n && !promise.isCanceled(); ++i) {
                const int page = (start + i) % n;
                const QList<QList<QRectF>> matches = doc->search(page, text, caseSensitive);
                if (!matches.isEmpty()) {
                    promise.addResult(PageHits{page, matches});
                }
            }
        }));
    }
    Q_EMIT searchChanged();
}

bool PdfView::isSearching() const
{
    return m_searchWatcher && m_searchWatcher->isRunning();
}

int PdfView::matchCount() const
{
    int n = 0;
    for (const QList<QList<QRectF>> &matches : m_hits) {
        n += matches.size();
    }
    return n;
}

int PdfView::currentMatch() const
{
    if (m_hitPage < 0 || m_hitIndex < 0) {
        return -1;
    }
    int n = 0;
    for (auto it = m_hits.constBegin(); it != m_hits.constEnd() && it.key() < m_hitPage; ++it) {
        n += it->size();
    }
    return n + m_hitIndex;
}

void PdfView::findNext(bool forward)
{
    if (m_hits.isEmpty()) {
        return;
    }
    auto it = m_hits.constFind(m_hitPage);
    if (it == m_hits.constEnd()) {
        it = m_hits.constBegin();
        m_hitIndex = forward ? -1 : it->size();
    }
    m_hitIndex += forward ? 1 : -1;
    if (m_hitIndex >= it->size()) {
        // On to the next page with matches, wrapping around at the end.
        ++it;
        if (it == m_hits.constEnd()) {
            it = m_hits.constBegin();
        }
        m_hitIndex = 0;
    } else if (m_hitIndex < 0) {
        if (it == m_hits.constBegin()) {
            it = m_hits.constEnd();
        }
        --it;
        m_hitIndex = it->size() - 1;
    }
    m_hitPage = it.key();
    showCurrentMatch();
    viewport()->update();
    Q_EMIT searchChanged();
}

// Scrolls only if the current match is not comfortably in view.
void PdfView::showCurrentMatch()
{
    const auto it = m_hits.constFind(m_hitPage);
    if (it == m_hits.constEnd() || m_hitIndex < 0 || m_hitIndex >= it->size()) {
        return;
    }
    const QRect r = toContent(m_hitPage, it->at(m_hitIndex).first()).toAlignedRect();
    QScrollBar *v = verticalScrollBar();
    QScrollBar *h = horizontalScrollBar();
    const QSize vp = viewport()->size();
    const int margin = 40;
    if (r.top() < v->value() + margin || r.bottom() > v->value() + vp.height() - margin) {
        v->setValue(r.center().y() - vp.height() / 3);
    }
    if (r.left() < h->value() + margin || r.right() > h->value() + vp.width() - margin) {
        h->setValue(r.center().x() - vp.width() / 2);
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
    // Scrolling or zooming a view makes it the one the window reports on and
    // keyboard commands go to, as a click would. Typing elsewhere, e.g. in the
    // find bar, is not interrupted.
    if (!hasFocus() && (!QApplication::focusWidget() || qobject_cast<PdfView *>(QApplication::focusWidget()))) {
        setFocus(Qt::MouseFocusReason);
    }
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
        if (hasTextSelection() || !m_selAnnots.isEmpty()) {
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
    if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        event->button() == Qt::BackButton ? goBack() : goForward();
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
        const bool shift = event->modifiers() & Qt::ShiftModifier;
        const bool additive = shift || (event->modifiers() & Qt::ControlModifier);
        if (const std::optional<AnnotationData> a = annotationAt(page, n, true)) {
            if (additive) {
                // Ctrl/Shift+click adds it to the selection or takes it out.
                clearTextSelection();
                QList<AnnotKey> keys = m_selAnnots;
                const AnnotKey key(a->page, a->id);
                if (!keys.removeOne(key)) {
                    keys.append(key);
                }
                setAnnotationSelection(keys);
                return;
            }
            // A click selects it; what a drag does is decided once it starts.
            m_pressAnnot = a;
            m_drag = Drag::PendingAnnot;
            return;
        }
        clearTextSelection();
        if (shift) {
            // Shift+drag selects annotations by area even over text.
            m_bandBase = m_selAnnots;
            m_bandOrigin = pos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
            m_drag = Drag::Band;
            return;
        }
        if (!additive) {
            selectAnnotation(-1, QString());
        }
        if (const std::optional<LinkData> link = linkAt(page, n, true)) {
            // A click follows it; a drag still selects the text or pans.
            m_pressLink = link;
            m_drag = Drag::PendingLink;
            return;
        }
        const int c = charAt(*m_doc->pageText(page), page, n, false);
        if (c >= 0 && !additive) {
            m_selPage = page;
            m_selAnchor = c;
            m_drag = Drag::SelectText;
        } else {
            // Blank paper: drag a rectangle to select annotations.
            m_bandBase = m_selAnnots;
            m_bandOrigin = pos + QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
            m_drag = Drag::Band;
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
    case Tool::Pen:
        selectAnnotation(-1, QString());
        clearTextSelection();
        m_strokePage = page;
        m_stroke = {n};
        m_drag = Drag::Draw;
        viewport()->update();
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
    case Drag::PendingLink: {
        if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            break;
        }
        m_pressLink.reset();
        int page;
        QPointF n;
        const int c = locate(m_pressPos, &page, &n) ? charAt(*m_doc->pageText(page), page, n, false) : -1;
        if (c < 0) {
            m_drag = Drag::Pan;
            m_panLast = event->globalPosition().toPoint() - (pos - m_pressPos);
            viewport()->setCursor(Qt::ClosedHandCursor);
            mouseMoveEvent(event);
            break;
        }
        m_selPage = page;
        m_selAnchor = c;
        m_drag = Drag::SelectText;
        mouseMoveEvent(event);
        break;
    }
    case Drag::PendingAnnot:
        if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            break;
        }
        if (!m_pressAnnot->isMarkup()) {
            // Dragging one of several selected annotations moves them all.
            if (!isAnnotationSelected(*m_pressAnnot)) {
                selectAnnotation(m_pressAnnot->page, m_pressAnnot->id);
            }
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
    case Drag::Band:
        updateBand();
        break;
    case Drag::Draw: {
        // The stroke stays on the page it began on.
        const QPointF raw = normalisedIn(m_strokePage, pos);
        const QPointF n(std::clamp(raw.x(), 0.0, 1.0), std::clamp(raw.y(), 0.0, 1.0));
        if (n != m_stroke.constLast()) {
            m_stroke.append(n);
            viewport()->update();
        }
        break;
    }
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
    case Drag::Draw:
        finishStroke();
        break;
    case Drag::PendingLink:
        if (const std::optional<LinkData> link = std::exchange(m_pressLink, std::nullopt)) {
            activateLink(*link);
        }
        break;
    case Drag::MoveAnnot: {
        const QPoint delta = pos - m_pressPos;
        QList<AnnotationData> list;
        for (const AnnotationData &a : selectedAnnotations()) {
            if (a.isMarkup()) {
                continue; // markup belongs to its text
            }
            const QRect r = m_rects.at(a.page);
            AnnotationData moved = a;
            moved.boundary.translate(double(delta.x()) / r.width(), double(delta.y()) / r.height());
            // Keep at least the top-left corner on the page.
            moved.boundary.moveTo(std::clamp(moved.boundary.x(), 0.0, 0.98), std::clamp(moved.boundary.y(), 0.0, 0.98));
            const QPointF shift = moved.boundary.topLeft() - a.boundary.topLeft();
            for (QList<QPointF> &path : moved.paths) {
                for (QPointF &point : path) {
                    point += shift;
                }
            }
            list.append(moved);
        }
        m_doc->updateAnnotations(list);
        break;
    }
    case Drag::Band:
        updateBand();
        m_bandBase.clear();
        break;
    default:
        break;
    }
    m_pressAnnot.reset();
    m_pressLink.reset();
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
    const std::optional<AnnotationData> a = annotationAt(page, n, true);
    const QList<AnnotationData> selected = selectedAnnotations();
    if (a && isAnnotationSelected(*a) && selected.size() > 1) {
        // Acts on the whole selection.
        QList<AnnotationData> highlights;
        for (const AnnotationData &d : selected) {
            if (d.kind == AnnotationData::Kind::Highlight) {
                highlights.append(d);
            }
        }
        if (!highlights.isEmpty()) {
            QMenu *colors = menu.addMenu(highlights.size() == 1 ? tr("Colour") : tr("Colour of %1 Highlights").arg(highlights.size()));
            for (const auto &[name, color] : highlightColors()) {
                colors->addAction(name, this, [this, highlights, color = color] {
                    QList<AnnotationData> list = highlights;
                    for (AnnotationData &d : list) {
                        d.color = color;
                    }
                    m_doc->updateAnnotations(list);
                });
            }
            menu.addSeparator();
        }
        menu.addAction(tr("Delete %1 Annotations").arg(selected.size()), this, [this] {
            deleteSelectedAnnotations();
        });
    } else if (a) {
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
            deleteSelectedAnnotations();
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
        menu.addAction(tr("Remove Page %1").arg(page + 1), this, [this, page] {
            Q_EMIT removePagesRequested({page});
        });
        menu.addAction(tr("Extract Page %1…").arg(page + 1), this, [this, page] {
            Q_EMIT extractPagesRequested({page});
        });
        menu.addSeparator();
        menu.addAction(tr("Rotate Page %1 Clockwise").arg(page + 1), this, [this, page] {
            Q_EMIT rotatePagesRequested({page}, 90);
        });
        menu.addAction(tr("Rotate Page %1 Counterclockwise").arg(page + 1), this, [this, page] {
            Q_EMIT rotatePagesRequested({page}, 270);
        });
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
        const std::optional<LinkData> link = m_tool == Tool::Select && !a ? linkAt(page, n, false) : std::nullopt;
        const auto text = m_doc->cachedPageText(page);
        const bool onText = text && charAt(*text, page, n, false) >= 0;
        switch (m_tool) {
        case Tool::Select:
            if (a || link) {
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
        case Tool::Pen:
            shape = Qt::CrossCursor;
            break;
        case Tool::Text:
            shape = Qt::IBeamCursor;
            break;
        }
        if (a && a->kind != AnnotationData::Kind::Text) {
            tip = a->contents;
        } else if (link) {
            switch (link->kind) {
            case LinkData::Kind::Goto:
                tip = tr("Go to page %1").arg(link->page + 1);
                break;
            case LinkData::Kind::Uri:
                tip = link->target;
                break;
            case LinkData::Kind::File:
                tip = link->page >= 0 ? tr("Open %1, page %2").arg(link->target).arg(link->page + 1) : tr("Open %1").arg(link->target);
                break;
            case LinkData::Kind::Action:
                break;
            }
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
