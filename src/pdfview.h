#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>
#include <QTimer>

#include <atomic>
#include <memory>

class Document;

// Continuous-scroll view of one document. Pages are rendered on worker
// threads and cached; a stale image is shown scaled while a sharp one is
// being produced, so zooming and scrolling never block.
class PdfView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class ZoomMode { Custom, FitWidth, FitPage };

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

Q_SIGNALS:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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

    std::shared_ptr<Document> m_doc;
    double m_zoom = 1.0;
    ZoomMode m_mode = ZoomMode::FitWidth;
    int m_currentPage = 0;

    QList<QRect> m_rects; // page rectangles in content coordinates
    QSize m_contentSize;

    QHash<int, QImage> m_cache;
    QHash<int, Pending> m_pending;
    QTimer m_renderTimer;

    bool m_panning = false;
    QPoint m_panLast;
};
