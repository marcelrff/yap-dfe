#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QSizeF>
#include <QString>

#include <memory>

namespace Poppler {
class Document;
}

// A loaded PDF. One instance is shared by every view showing the same file.
// Page sizes are cached at load time so the GUI thread never touches Poppler
// afterwards; render() may be called from any thread.
class Document
{
public:
    enum class LoadStatus { Ok, Locked, Failed };

    static std::shared_ptr<Document> load(const QString &path, const QString &password, LoadStatus *status);

    ~Document();

    QString path() const { return m_path; }
    QString fileName() const;
    int pageCount() const { return m_sizes.size(); }
    QSizeF pageSize(int page) const { return m_sizes.at(page); } // in points

    // Renders a whole page into an image of exactly pixelSize. Thread-safe.
    QImage render(int page, QSize pixelSize);

private:
    Document();

    QString m_path;
    std::unique_ptr<Poppler::Document> m_doc;
    QList<QSizeF> m_sizes;
    QMutex m_mutex;
};
