#include "document.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <QUuid>

#include <poppler-annotation.h>
#include <poppler-converter.h>
#include <poppler-qt6.h>

#include <filesystem>

namespace {
const QSet<Poppler::Annotation::SubType> SupportedTypes{Poppler::Annotation::AText, Poppler::Annotation::AHighlight};

QString newId()
{
    return QStringLiteral("yapdfv-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

Poppler::HighlightAnnotation::HighlightType toPoppler(AnnotationData::Kind kind)
{
    switch (kind) {
    case AnnotationData::Kind::Underline:
        return Poppler::HighlightAnnotation::Underline;
    case AnnotationData::Kind::StrikeOut:
        return Poppler::HighlightAnnotation::StrikeOut;
    case AnnotationData::Kind::Squiggly:
        return Poppler::HighlightAnnotation::Squiggly;
    default:
        return Poppler::HighlightAnnotation::Highlight;
    }
}

AnnotationData::Kind fromPoppler(Poppler::HighlightAnnotation::HighlightType type)
{
    switch (type) {
    case Poppler::HighlightAnnotation::Underline:
        return AnnotationData::Kind::Underline;
    case Poppler::HighlightAnnotation::StrikeOut:
        return AnnotationData::Kind::StrikeOut;
    case Poppler::HighlightAnnotation::Squiggly:
        return AnnotationData::Kind::Squiggly;
    default:
        return AnnotationData::Kind::Highlight;
    }
}

QList<Poppler::HighlightAnnotation::Quad> toQuads(const QList<QRectF> &rects)
{
    QList<Poppler::HighlightAnnotation::Quad> quads;
    for (const QRectF &r : rects) {
        Poppler::HighlightAnnotation::Quad q;
        q.points[0] = r.topLeft();
        q.points[1] = r.topRight();
        q.points[2] = r.bottomRight();
        q.points[3] = r.bottomLeft();
        q.capStart = false;
        q.capEnd = false;
        q.feather = 0;
        quads.append(q);
    }
    return quads;
}

QFont textFont(double pointSize)
{
    QFont font(Document::textFontFamily());
    font.setPointSizeF(pointSize);
    return font;
}

// Writes the fields that both a new and an edited annotation carry.
void writeCommon(Poppler::Annotation &ann, const AnnotationData &d)
{
    ann.setBoundary(d.boundary);
    ann.setContents(d.contents);
    ann.setModificationDate(QDateTime::currentDateTime());

    Poppler::Annotation::Style style = ann.style();
    if (d.kind == AnnotationData::Kind::Text) {
        // For in-place text the style colour is the box fill; leave it unset
        // so the page shows through. The text colour is set separately.
        style.setWidth(0);
    } else {
        style.setColor(d.color);
    }
    ann.setStyle(style);

    if (d.isMarkup()) {
        auto &h = static_cast<Poppler::HighlightAnnotation &>(ann);
        h.setHighlightType(toPoppler(d.kind));
        h.setHighlightQuads(toQuads(d.rects));
    } else if (d.kind == AnnotationData::Kind::Text) {
        auto &t = static_cast<Poppler::TextAnnotation &>(ann);
        t.setTextColor(d.color);
        t.setTextFont(textFont(d.fontSize));
    }
}

std::unique_ptr<Poppler::Annotation> createAnnotation(const AnnotationData &d)
{
    std::unique_ptr<Poppler::Annotation> ann;
    if (d.isMarkup()) {
        ann = std::make_unique<Poppler::HighlightAnnotation>();
    } else if (d.kind == AnnotationData::Kind::Note) {
        auto note = std::make_unique<Poppler::TextAnnotation>(Poppler::TextAnnotation::Linked);
        note->setTextIcon(QStringLiteral("Comment"));
        ann = std::move(note);
    } else {
        auto text = std::make_unique<Poppler::TextAnnotation>(Poppler::TextAnnotation::InPlace);
        text->setInplaceIntent(Poppler::TextAnnotation::TypeWriter);
        text->setInplaceAlign(Poppler::TextAnnotation::InplaceAlignLeft);
        ann = std::move(text);
    }
    ann->setUniqueName(d.id);
    ann->setCreationDate(QDateTime::currentDateTime());
    writeCommon(*ann, d);
    return ann;
}

std::unique_ptr<Poppler::Annotation> findAnnotation(const Poppler::Page &page, const QString &id)
{
    for (std::unique_ptr<Poppler::Annotation> &ann : page.annotations(SupportedTypes)) {
        if (ann->uniqueName() == id) {
            return std::move(ann);
        }
    }
    return nullptr;
}

PageText readText(const Poppler::Page &page, QSizeF pt)
{
    PageText chars;
    const auto normalise = [pt](const QRectF &r) {
        return QRectF(r.x() / pt.width(), r.y() / pt.height(), r.width() / pt.width(), r.height() / pt.height());
    };
    const std::vector<std::unique_ptr<Poppler::TextBox>> boxes = page.textList();
    int word = 0;
    for (size_t b = 0; b < boxes.size(); ++b) {
        const std::unique_ptr<Poppler::TextBox> &box = boxes[b];
        const QString text = box->text();
        for (int i = 0; i < text.size(); ++i) {
            QRectF r = box->charBoundingBox(i);
            if (r.width() <= 0 || r.height() <= 0) {
                r = box->boundingBox();
            }
            chars.append({normalise(r), text.mid(i, 1), word, false, false});
        }
        if (!text.isEmpty()) {
            const bool lineEnd = box->nextWord() == nullptr;
            bool space = box->hasSpaceAfter();
            // Poppler does not flag a space that is an actual glyph in the
            // PDF; a visible gap to the next word on the line counts as one.
            if (!space && !lineEnd && b + 1 < boxes.size()) {
                const QRectF here = box->boundingBox();
                space = boxes[b + 1]->boundingBox().left() - here.right() > here.height() * 0.08;
            }
            chars.last().spaceAfter = space;
            chars.last().breakAfter = lineEnd;
        }
        ++word;
    }
    return chars;
}
}

// One undoable annotation change: `before` and `after` are the annotation's
// state on either side of it; a null id means "does not exist".
class AnnotationCommand : public QUndoCommand
{
public:
    AnnotationCommand(Document *doc, const AnnotationData &before, const AnnotationData &after, const QString &text)
        : QUndoCommand(text)
        , m_doc(doc)
        , m_before(before)
        , m_after(after)
    {
    }

    void redo() override { apply(m_before, m_after); }
    void undo() override { apply(m_after, m_before); }

private:
    void apply(const AnnotationData &from, const AnnotationData &to)
    {
        if (from.id.isEmpty()) {
            m_doc->applyAdd(to);
        } else if (to.id.isEmpty()) {
            m_doc->applyRemove(from.page, from.id);
        } else {
            m_doc->applyUpdate(to);
        }
    }

    Document *m_doc;
    AnnotationData m_before;
    AnnotationData m_after;
};

Document::Document()
{
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        Q_EMIT modifiedChanged(!clean);
    });
}

Document::~Document() = default;

QString Document::textFontFamily()
{
    static const QString family = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    return family;
}

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

    // Render jobs on worker threads may hold the last reference; the object
    // itself must still be destroyed on its own (GUI) thread.
    std::shared_ptr<Document> result(new Document, [](Document *d) {
        d->deleteLater();
    });
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
    QImage image = p->renderToImage(xres, yres, 0, 0, pixelSize.width(), pixelSize.height());
    // The page is about to be looked at: get its text and annotations ready
    // here so the GUI thread does not have to wait for them later.
    primeCaches(page, *p);
    return image;
}

// --- text and annotation caches ---------------------------------------------

void Document::primeCaches(int page, const Poppler::Page &p)
{
    bool haveText, haveAnnots;
    {
        QMutexLocker cache(&m_cacheMutex);
        haveText = m_text.contains(page);
        haveAnnots = m_annots.contains(page);
    }
    if (!haveText) {
        auto text = std::make_shared<const PageText>(readText(p, m_sizes.at(page)));
        QMutexLocker cache(&m_cacheMutex);
        m_text.insert(page, std::move(text));
    }
    if (!haveAnnots) {
        refreshAnnotations(page, p);
    }
}

void Document::refreshAnnotations(int page, const Poppler::Page &p)
{
    QList<AnnotationData> list;
    for (const std::unique_ptr<Poppler::Annotation> &ann : p.annotations(SupportedTypes)) {
        if (ann->flags() & Poppler::Annotation::Hidden) {
            continue;
        }
        // Annotations from other programs often have no name; give them one
        // so they can be addressed later.
        if (ann->uniqueName().isEmpty()) {
            ann->setUniqueName(newId());
        }
        AnnotationData d;
        d.id = ann->uniqueName();
        d.page = page;
        d.boundary = ann->boundary();
        d.contents = ann->contents();
        d.color = ann->style().color();
        if (ann->subType() == Poppler::Annotation::AHighlight) {
            const auto *h = static_cast<const Poppler::HighlightAnnotation *>(ann.get());
            d.kind = fromPoppler(h->highlightType());
            for (const Poppler::HighlightAnnotation::Quad &q : h->highlightQuads()) {
                d.rects.append(QPolygonF({q.points[0], q.points[1], q.points[2], q.points[3]}).boundingRect());
            }
            if (d.rects.isEmpty()) {
                d.rects.append(d.boundary);
            }
        } else {
            const auto *t = static_cast<const Poppler::TextAnnotation *>(ann.get());
            if (t->textType() == Poppler::TextAnnotation::InPlace) {
                d.kind = AnnotationData::Kind::Text;
                d.color = t->textColor();
                d.fontSize = t->textFont().pointSizeF();
            } else {
                d.kind = AnnotationData::Kind::Note;
            }
        }
        list.append(d);
    }
    QMutexLocker cache(&m_cacheMutex);
    m_annots.insert(page, list);
}

std::shared_ptr<const PageText> Document::cachedPageText(int page) const
{
    QMutexLocker cache(&m_cacheMutex);
    return m_text.value(page);
}

std::shared_ptr<const PageText> Document::pageText(int page)
{
    if (auto text = cachedPageText(page)) {
        return text;
    }
    QMutexLocker lock(&m_mutex);
    if (const std::unique_ptr<Poppler::Page> p = m_doc->page(page)) {
        primeCaches(page, *p);
    }
    lock.unlock();
    auto text = cachedPageText(page);
    return text ? text : std::make_shared<const PageText>();
}

bool Document::cachedAnnotations(int page, QList<AnnotationData> *out) const
{
    QMutexLocker cache(&m_cacheMutex);
    const auto it = m_annots.constFind(page);
    if (it == m_annots.constEnd()) {
        return false;
    }
    *out = *it;
    return true;
}

QList<AnnotationData> Document::annotations(int page)
{
    QList<AnnotationData> list;
    if (cachedAnnotations(page, &list)) {
        return list;
    }
    QMutexLocker lock(&m_mutex);
    if (const std::unique_ptr<Poppler::Page> p = m_doc->page(page)) {
        primeCaches(page, *p);
    }
    lock.unlock();
    cachedAnnotations(page, &list);
    return list;
}

QList<QList<QRectF>> Document::search(int page, const QString &text, bool caseSensitive)
{
    QList<QList<QRectF>> matches;
    if (page < 0 || page >= m_sizes.size() || text.isEmpty()) {
        return matches;
    }
    QMutexLocker lock(&m_mutex);
    const std::unique_ptr<Poppler::Page> p = m_doc->page(page);
    if (!p) {
        return matches;
    }
    const QSizeF pt = m_sizes.at(page);
    const auto normalise = [pt](const QRectF &r) {
        return QRectF(r.x() / pt.width(), r.y() / pt.height(), r.width() / pt.width(), r.height() / pt.height());
    };
    const Poppler::Page::SearchFlags flags = caseSensitive ? Poppler::Page::NoSearchFlags : Poppler::Page::IgnoreCase;

    // Searching across lines also finds words hyphenated at a line end, but
    // Poppler reports such a match as two separate rectangles. Comparing with
    // a plain search tells the halves apart: whatever the plain search did
    // not find is half of a cross-line match, and the halves come in pairs.
    const QList<QRectF> plain = p->search(text, flags);
    const QList<QRectF> all = p->search(text, flags | Poppler::Page::AcrossLines);
    const auto isPlain = [&plain](const QRectF &r) {
        for (const QRectF &q : plain) {
            if (qAbs(q.x() - r.x()) < 0.5 && qAbs(q.y() - r.y()) < 0.5 && qAbs(q.width() - r.width()) < 0.5) {
                return true;
            }
        }
        return false;
    };
    for (int i = 0; i < all.size(); ++i) {
        QList<QRectF> match{normalise(all.at(i))};
        if (!isPlain(all.at(i)) && i + 1 < all.size() && !isPlain(all.at(i + 1))) {
            match.append(normalise(all.at(++i)));
        }
        matches.append(match);
    }
    return matches;
}

// --- editing ------------------------------------------------------------------

QString Document::addAnnotation(AnnotationData data)
{
    data.id = newId();
    m_undo.push(new AnnotationCommand(this, {}, data, tr("Add annotation")));
    return data.id;
}

void Document::removeAnnotation(int page, const QString &id)
{
    for (const AnnotationData &d : annotations(page)) {
        if (d.id == id) {
            m_undo.push(new AnnotationCommand(this, d, {}, tr("Delete annotation")));
            return;
        }
    }
}

void Document::updateAnnotation(const AnnotationData &data)
{
    for (const AnnotationData &d : annotations(data.page)) {
        if (d.id == data.id) {
            if (d != data) {
                m_undo.push(new AnnotationCommand(this, d, data, tr("Edit annotation")));
            }
            return;
        }
    }
}

void Document::applyAdd(const AnnotationData &data)
{
    {
        QMutexLocker lock(&m_mutex);
        const std::unique_ptr<Poppler::Page> p = m_doc->page(data.page);
        if (!p) {
            return;
        }
        const std::unique_ptr<Poppler::Annotation> ann = createAnnotation(data);
        p->addAnnotation(ann.get());
        refreshAnnotations(data.page, *p);
    }
    Q_EMIT pageChanged(data.page);
}

void Document::applyRemove(int page, const QString &id)
{
    {
        QMutexLocker lock(&m_mutex);
        const std::unique_ptr<Poppler::Page> p = m_doc->page(page);
        if (!p) {
            return;
        }
        if (std::unique_ptr<Poppler::Annotation> ann = findAnnotation(*p, id)) {
            p->removeAnnotation(ann.release()); // Poppler destroys the object
        }
        refreshAnnotations(page, *p);
    }
    Q_EMIT pageChanged(page);
}

void Document::applyUpdate(const AnnotationData &data)
{
    {
        QMutexLocker lock(&m_mutex);
        const std::unique_ptr<Poppler::Page> p = m_doc->page(data.page);
        if (!p) {
            return;
        }
        if (const std::unique_ptr<Poppler::Annotation> ann = findAnnotation(*p, data.id)) {
            writeCommon(*ann, data);
        }
        refreshAnnotations(data.page, *p);
    }
    Q_EMIT pageChanged(data.page);
}

// --- saving -------------------------------------------------------------------

bool Document::save(const QString &path, QString *error)
{
    QFileInfo info(path);
    // Write through a symlink rather than replacing it.
    const QString target = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
    info = QFileInfo(target);

    // Poppler still reads from the original file, so never write over it in
    // place: save next to it, then swap the new file in atomically.
    QTemporaryFile temp(info.absolutePath() + QStringLiteral("/.") + info.fileName() + QStringLiteral(".XXXXXX"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        *error = tr("Cannot write to %1.").arg(info.absolutePath());
        return false;
    }
    const QString tempName = temp.fileName();
    temp.close();

    bool ok;
    {
        QMutexLocker lock(&m_mutex);
        const std::unique_ptr<Poppler::PDFConverter> converter = m_doc->pdfConverter();
        converter->setOutputFileName(tempName);
        converter->setPDFOptions(Poppler::PDFConverter::WithChanges);
        ok = converter->convert();
    }
    if (!ok) {
        QFile::remove(tempName);
        *error = tr("The document could not be written.");
        return false;
    }

    const QFile::Permissions permissions = info.exists()
        ? info.permissions()
        : QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
    QFile::setPermissions(tempName, permissions);

    std::error_code ec;
    std::filesystem::rename(tempName.toStdString(), target.toStdString(), ec);
    if (ec) {
        QFile::remove(tempName);
        *error = tr("Cannot replace %1: %2").arg(target, QString::fromStdString(ec.message()));
        return false;
    }

    m_undo.setClean();
    if (target != m_path) {
        m_path = target;
        Q_EMIT pathChanged();
    }
    return true;
}
