#include "document.h"

#include <QFileInfo>
#include <QMutexLocker>

#include <poppler-qt6.h>

Document::Document() = default;
Document::~Document() = default;

std::shared_ptr<Document> Document::load(const QString &path, const QString &password, LoadStatus *status)
{
    const QByteArray pw = password.toUtf8();
    std::unique_ptr<Poppler::Document> doc = Poppler::Document::load(path, pw, pw);
    if (!doc) {
        *status = LoadStatus::Failed;
        return nullptr;
    }
    if (doc->isLocked()) {
        *status = LoadStatus::Locked;
        return nullptr;
    }

    doc->setRenderHint(Poppler::Document::Antialiasing);
    doc->setRenderHint(Poppler::Document::TextAntialiasing);
    doc->setRenderHint(Poppler::Document::TextHinting, false);

    std::shared_ptr<Document> result(new Document);
    result->m_path = QFileInfo(path).absoluteFilePath();
    const int n = doc->numPages();
    result->m_sizes.reserve(n);
    for (int i = 0; i < n; ++i) {
        const std::unique_ptr<Poppler::Page> page = doc->page(i);
        QSizeF size = page ? page->pageSizeF() : QSizeF();
        if (size.width() < 1 || size.height() < 1) {
            size = QSizeF(595, 842); // broken page: fall back to A4 so layout stays sane
        }
        result->m_sizes.append(size);
    }
    if (n == 0) {
        *status = LoadStatus::Failed;
        return nullptr;
    }
    result->m_doc = std::move(doc);
    *status = LoadStatus::Ok;
    return result;
}

QString Document::fileName() const
{
    return QFileInfo(m_path).fileName();
}

QImage Document::render(int page, QSize pixelSize)
{
    if (page < 0 || page >= m_sizes.size() || pixelSize.isEmpty()) {
        return {};
    }
    QMutexLocker lock(&m_mutex);
    const std::unique_ptr<Poppler::Page> p = m_doc->page(page);
    if (!p) {
        return {};
    }
    const QSizeF pt = m_sizes.at(page);
    const double xres = 72.0 * pixelSize.width() / pt.width();
    const double yres = 72.0 * pixelSize.height() / pt.height();
    return p->renderToImage(xres, yres, 0, 0, pixelSize.width(), pixelSize.height());
}
