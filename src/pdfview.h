#pragma once

#include "document.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QList>
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
    enum class Tool { Select, Highlight, Underline, StrikeOut, Note, Text };

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

    bool isRendering() const { return !m_pending.isEmpty() || m_renderTimer.isActive(); }

    // --- tools, selection, annotations ---
    Tool tool() const { return m_tool; }
    void setTool(Tool tool, const QColor &color);

    bool hasTextSelection() const;
    QString selectedText() const;
    void clearTextSelection();
    void copySelection() const;
    // Turns the current text selection into a markup annotation.
    bool markupSelection(AnnotationData::Kind kind, const QColor &color);

    QString selectedAnnotation() const { return m_selAnnotId; }
    bool deleteSelectedAnnotation();

    // Viewport position of a normalised point on a page.
    QPoint viewportPoint(int page, QPointF normalised) const;

    static QList<QPair<QString, QColor>> highlightColors();
    static QColor lineColor(); // underline and strike-out
    static QColor noteColor();

    // Asks the user for a comment; returns false if cancelled. Replaceable so
    // tests can answer without a dialog.
    static std::function<bool(QWidget *parent, QString *text)> commentEditor;

Q_SIGNALS:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    void selectionChanged();
    void toolFinished(); // the view wants to go back to the Select tool

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
    };
    enum class Drag { None, Pan, SelectText, PendingAnnot, MoveAnnot };

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

    // geometry helpers
    bool locate(QPoint viewportPos, int *page, QPointF *normalised) const;
    QPointF normalisedIn(int page, QPoint viewportPos) const;
    QRectF toContent(int page, const QRectF &normalised) const;

    // text
    int charAt(const PageText &text, int page, QPointF normalised, bool nearest) const;
    static QList<QRectF> lineRects(const PageText &text, int from, int to);
    QPair<int, int> selectionRange() const;

    // annotations
    std::optional<AnnotationData> annotationAt(int page, QPointF normalised, bool blocking) const;
    std::optional<AnnotationData> selectedAnnotationData() const;
    void selectAnnotation(int page, const QString &id);
    void editAnnotation(const AnnotationData &annotation);
    void createNote(int page, QPointF normalised);
    void openEditor(int page, QPointF topLeft, const std::optional<AnnotationData> &existing);
    void finishEditor(bool accept);
    void paintMarkup(QPainter &p, int page, AnnotationData::Kind kind, const QColor &color, const QList<QRectF> &rects) const;
    void updateCursor(QPoint viewportPos);
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

    int m_selPage = -1; // text selection: page, and inclusive character range
    int m_selAnchor = -1;
    int m_selCursor = -1;

    int m_selAnnotPage = -1;
    QString m_selAnnotId;

    QPointer<InlineEditor> m_editor;
    int m_editPage = 0;
    std::optional<AnnotationData> m_editExisting;
};
