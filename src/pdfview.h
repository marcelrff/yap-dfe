#pragma once

#include "document.h"

#include <QAbstractScrollArea>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class InlineEditor;

// Continuous-scroll view of one document. Pages are rendered on worker
// threads and cached; a stale image is shown scaled while a sharp one is
// being produced, so zooming and scrolling never block.
//
// The view also owns the interaction: text selection, and creating, selecting,
// moving and editing annotations according to the current tool.
class PdfView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class ZoomMode { Custom, FitWidth, FitPage };
    enum class Tool { Select, Highlight, Underline, StrikeOut, Note, Text, Pen };

    // Enough to put another view at the same place in the same document.
    struct ViewState {
        ZoomMode mode = ZoomMode::FitWidth;
        double zoom = 1.0;
        int page = 0;
        double fx = 0.0; // position of the viewport's top-left corner,
        double fy = 0.0; // as a fraction of that page's size
    };

    explicit PdfView(std::shared_ptr<Document> doc, QWidget *parent = nullptr);
    ~PdfView() override;

    std::shared_ptr<Document> document() const { return m_doc; }

    double zoom() const { return m_zoom; }
    ZoomMode zoomMode() const { return m_mode; }
    void setZoom(double zoom);
    void setZoomMode(ZoomMode mode);
    void zoomIn();
    void zoomOut();

    int currentPage() const { return m_currentPage; }
    int pageCount() const;
    void goToPage(int page);

    ViewState viewState() const;
    void restoreViewState(const ViewState &state);

    // A jump the user may want to undo: remembered for goBack(). `left` and
    // `top` are a normalised position on the page; negative leaves it open.
    void jumpTo(int page, double left = -1, double top = -1);
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool canGoForward() const { return !m_forward.isEmpty(); }
    void goBack();
    void goForward();

    // Follows a link as if it had been clicked.
    void activateLink(const LinkData &link);

    bool isRendering() const { return !m_pending.isEmpty() || m_renderTimer.isActive(); }

    // --- tools, selection, annotations ---
    Tool tool() const { return m_tool; }
    void setTool(Tool tool, const QColor &color);

    bool hasTextSelection() const;
    QString selectedText() const;
    void clearTextSelection();
    void copySelection() const;
    // Deletes the selected text from the PDF itself (undoable).
    bool removeSelectedText(QString *error);
    // Turns the current text selection into a markup annotation.
    bool markupSelection(AnnotationData::Kind kind, const QColor &color);

    // Annotation selection: a click picks one, Ctrl/Shift+click adds or
    // removes one, a drag from blank paper (or Shift+drag anywhere) selects
    // everything the rectangle touches.
    QString selectedAnnotation() const; // the first selected, or empty
    QList<AnnotationData> selectedAnnotations() const;
    void selectAllAnnotations();
    bool deleteSelectedAnnotations();

    // --- search ---
    // Starts searching the whole document in the background; matches come
    // in page by page, beginning at the current page. Empty text clears.
    void setSearch(const QString &text, bool caseSensitive);
    void findNext(bool forward);
    QString searchText() const { return m_searchText; }
    int matchCount() const;
    int currentMatch() const; // 0-based position among all matches, -1 if none
    bool isSearching() const;

    // Dark pages: lightness is inverted, colours keep their hue.
    void setDarkMode(bool dark);
    bool darkMode() const { return m_dark; }
    static QColor darkened(const QColor &color);

    // Viewport position of a normalised point on a page.
    QPoint viewportPoint(int page, QPointF normalised) const;

    static QList<QPair<QString, QColor>> highlightColors();
    static QColor lineColor(); // underline and strike-out
    static QColor noteColor();
    static QColor penColor();

    // Asks the user for a comment; returns false if cancelled. Replaceable so
    // tests can answer without a dialog.
    static std::function<bool(QWidget *parent, QString *text)> commentEditor;
    // Opens a link to a web address or another program. Replaceable so tests
    // can see what would have been opened.
    static std::function<void(QWidget *parent, const QUrl &url)> urlOpener;

Q_SIGNALS:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    void selectionChanged();
    void searchChanged(); // match count, current match or progress changed
    void toolFinished(); // the view wants to go back to the Select tool
    void historyChanged(); // canGoBack() or canGoForward() changed
    // A link points into another PDF; `page` is -1 if it names none.
    void openFileRequested(const QString &path, int page);
    // The page menu asks to remove or extract pages (0-based).
    void removePagesRequested(const QList<int> &pages);
    void extractPagesRequested(const QList<int> &pages);
    void rotatePagesRequested(const QList<int> &pages, int degrees); // clockwise
    void removeTextRequested(); // the selection menu asks to delete the selected text

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Anchor {
        int page = 0;
        double fx = 0.0;
        double fy = 0.0;
    };
    struct Pending {
        QSize size;
        std::shared_ptr<std::atomic_bool> cancel;
    };
    // Markup painted by the view itself until the re-rendered page arrives.
    struct Overlay {
        AnnotationData::Kind kind;
        QColor color;
        QList<QRectF> rects;
        QList<QPointF> stroke = {}; // Ink: the pen stroke
    };
    enum class Drag { None, Pan, SelectText, PendingAnnot, MoveAnnot, PendingLink, Draw, Band };
    using AnnotKey = QPair<int, QString>; // page, id

    double pxPerPoint() const;
    void relayout();
    void updateScrollBars();
    int pageAtY(int y) const;
    QPair<int, int> visibleRange() const; // first, last (inclusive)
    Anchor captureAnchor(QPoint viewportPos) const;
    void restoreAnchor(const Anchor &anchor, QPoint viewportPos);
    void applyZoom(double zoom, QPoint anchorPos);
    void applyFit();
    void updateCurrentPage();
    QSize renderSize(int page) const;
    void requestRenders(int delayMs);
    void startRenders();
    void onPageChanged(int page);
    void onPagesChanged();

    // geometry helpers
    bool locate(QPoint viewportPos, int *page, QPointF *normalised) const;
    QPointF normalisedIn(int page, QPoint viewportPos) const;
    QRectF toContent(int page, const QRectF &normalised) const;

    // text
    int charAt(const PageText &text, int page, QPointF normalised, bool nearest) const;
    static QList<QRectF> lineRects(const PageText &text, int from, int to);
    QPair<int, int> selectionRange() const;

    // links
    std::optional<LinkData> linkAt(int page, QPointF normalised, bool blocking) const;
    void scrollTo(int page, double left, double top);
    void restorePosition(const Anchor &anchor);

    // annotations
    std::optional<AnnotationData> annotationAt(int page, QPointF normalised, bool blocking) const;
    void selectAnnotation(int page, const QString &id); // just this one; an empty id clears
    void setAnnotationSelection(const QList<AnnotKey> &keys);
    bool isAnnotationSelected(const AnnotationData &a) const;
    QRect bandRect() const; // content coordinates
    QList<AnnotKey> annotationsIn(const QRect &content) const;
    void updateBand();
    void editAnnotation(const AnnotationData &annotation);
    void createNote(int page, QPointF normalised);
    void openEditor(int page, QPointF topLeft, const std::optional<AnnotationData> &existing);
    void finishEditor(bool accept);
    void paintStroke(QPainter &p, int page, const QColor &color, const QList<QPointF> &stroke) const;
    void finishStroke();
    void paintMarkup(QPainter &p, int page, AnnotationData::Kind kind, const QColor &color, const QList<QRectF> &rects) const;
    void updateCursor(QPoint viewportPos);
    void cancelSearch();
    void showCurrentMatch();
    void startPan(QMouseEvent *event);

    std::shared_ptr<Document> m_doc;
    double m_zoom = 1.0;
    ZoomMode m_mode = ZoomMode::FitWidth;
    int m_currentPage = 0;

    QList<QRect> m_rects; // page rectangles in content coordinates
    QSize m_contentSize;

    QHash<int, QImage> m_cache;
    QSet<int> m_stale; // cached pages whose content changed
    QHash<int, Pending> m_pending;
    QHash<int, QList<Overlay>> m_overlays;
    QTimer m_renderTimer;

    Tool m_tool = Tool::Select;
    QColor m_toolColor;

    Drag m_drag = Drag::None;
    QPoint m_pressPos;
    QPoint m_lastPos;
    QPoint m_panLast;
    std::optional<AnnotationData> m_pressAnnot;
    std::optional<LinkData> m_pressLink;
    int m_strokePage = -1; // the pen stroke being drawn
    QList<QPointF> m_stroke; // normalised

    QList<Anchor> m_back; // positions before each jump, the latest last
    QList<Anchor> m_forward;

    int m_selPage = -1; // text selection: page, and inclusive character range
    int m_selAnchor = -1;
    int m_selCursor = -1;

    QList<AnnotKey> m_selAnnots;
    QList<AnnotKey> m_bandBase; // selection kept while a band is dragged
    QPoint m_bandOrigin; // content coordinates

    struct PageHits {
        int page;
        QList<QList<QRectF>> matches;
    };
    QString m_searchText;
    bool m_searchCase = false;
    QMap<int, QList<QList<QRectF>>> m_hits; // pages with matches, in page order
    int m_hitPage = -1; // current match
    int m_hitIndex = -1;
    QFutureWatcher<PageHits> *m_searchWatcher = nullptr;

    bool m_dark = false;

    QPointer<InlineEditor> m_editor;
    int m_editPage = 0;
    std::optional<AnnotationData> m_editExisting;
};
