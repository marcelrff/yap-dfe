#include "document.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <QUuid>

#include <poppler-annotation.h>
#include <poppler-converter.h>
#include <poppler-link.h>
#include <poppler-qt6.h>

#include <qpdf/QPDFJob.hh>

#include <algorithm>
#include <filesystem>

namespace {
const QSet<Poppler::Annotation::SubType> SupportedTypes{Poppler::Annotation::AText, Poppler::Annotation::AHighlight, Poppler::Annotation::AInk};

QString newId()
{
    return QStringLiteral("yapdfe-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
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
        if (d.kind == AnnotationData::Kind::Ink) {
            style.setWidth(d.width);
        }
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
    } else if (d.kind == AnnotationData::Kind::Ink) {
        static_cast<Poppler::InkAnnotation &>(ann).setInkPaths(d.paths);
    }
}

std::unique_ptr<Poppler::Annotation> createAnnotation(const AnnotationData &d)
{
    std::unique_ptr<Poppler::Annotation> ann;
    if (d.isMarkup()) {
        ann = std::make_unique<Poppler::HighlightAnnotation>();
    } else if (d.kind == AnnotationData::Kind::Ink) {
        ann = std::make_unique<Poppler::InkAnnotation>();
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

// Annotations from other programs often have no name. Address those by an id
// derived from their content instead of writing a name into them: changing only
// the dictionary of an annotation whose appearance stream still lives in the
// file makes Poppler 26.11's font subsetting use freed memory on save
// (AnnotFreeText::subsetFonts), and it would mark a file modified just by
// viewing it. The id becomes the annotation's real name once it is edited.
QString annotationId(const Poppler::Annotation &ann)
{
    const QString name = ann.uniqueName();
    if (!name.isEmpty()) {
        return name;
    }
    const QRectF b = ann.boundary();
    QStringList key{QString::number(int(ann.subType())),
                    QString::number(b.x(), 'g', 17),
                    QString::number(b.y(), 'g', 17),
                    QString::number(b.width(), 'g', 17),
                    QString::number(b.height(), 'g', 17),
                    ann.contents(),
                    ann.author(),
                    ann.creationDate().toString(Qt::ISODateWithMs)};
    const QByteArray hash = QCryptographicHash::hash(key.join(QChar(0x1f)).toUtf8(), QCryptographicHash::Sha1);
    return QStringLiteral("yapdfe-anon-") + QString::fromLatin1(hash.toHex().left(16));
}

std::unique_ptr<Poppler::Annotation> findAnnotation(const Poppler::Page &page, const QString &id)
{
    for (std::unique_ptr<Poppler::Annotation> &ann : page.annotations(SupportedTypes)) {
        if (annotationId(*ann) == id) {
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

QList<LinkData> readLinks(const Poppler::Page &page, Poppler::Document &doc)
{
    QList<LinkData> links;
    for (const std::unique_ptr<Poppler::Link> &link : page.links()) {
        LinkData d;
        // Some producers write the rectangle upside down.
        d.area = link->linkArea().normalized();
        if (d.area.isEmpty()) {
            continue;
        }
        switch (link->linkType()) {
        case Poppler::Link::Goto: {
            const auto *go = static_cast<const Poppler::LinkGoto *>(link.get());
            Poppler::LinkDestination dest = go->destination();
            if (go->isExternal()) {
                d.kind = LinkData::Kind::File;
                d.target = go->fileName();
                d.page = dest.pageNumber() - 1;
                break;
            }
            // Named destinations (as LaTeX writes them) are looked up here.
            if (dest.pageNumber() <= 0 && !dest.destinationName().isEmpty()) {
                if (const std::unique_ptr<Poppler::LinkDestination> named = doc.linkDestination(dest.destinationName())) {
                    dest = *named;
                }
            }
            if (dest.pageNumber() <= 0 || dest.pageNumber() > doc.numPages()) {
                continue;
            }
            d.kind = LinkData::Kind::Goto;
            d.page = dest.pageNumber() - 1;
            if (dest.isChangeLeft()) {
                d.left = std::clamp(dest.left(), 0.0, 1.0);
            }
            if (dest.isChangeTop()) {
                d.top = std::clamp(dest.top(), 0.0, 1.0);
            }
            break;
        }
        case Poppler::Link::Browse:
            d.kind = LinkData::Kind::Uri;
            d.target = static_cast<const Poppler::LinkBrowse *>(link.get())->url().trimmed();
            if (d.target.isEmpty()) {
                continue;
            }
            break;
        case Poppler::Link::Action:
            d.kind = LinkData::Kind::Action;
            switch (static_cast<const Poppler::LinkAction *>(link.get())->actionType()) {
            case Poppler::LinkAction::PageFirst:
                d.action = LinkData::Action::FirstPage;
                break;
            case Poppler::LinkAction::PagePrev:
                d.action = LinkData::Action::PreviousPage;
                break;
            case Poppler::LinkAction::PageNext:
                d.action = LinkData::Action::NextPage;
                break;
            case Poppler::LinkAction::PageLast:
                d.action = LinkData::Action::LastPage;
                break;
            case Poppler::LinkAction::HistoryBack:
                d.action = LinkData::Action::Back;
                break;
            case Poppler::LinkAction::HistoryForward:
                d.action = LinkData::Action::Forward;
                break;
            default:
                continue;
            }
            break;
        default:
            // Scripts, programs to run, media, forms: not supported.
            continue;
        }
        links.append(d);
    }
    return links;
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
    result->m_password = password;
    result->m_pageCount = n;
    *status = LoadStatus::Ok;
    return result;
}

QString Document::fileName() const
{
    return QFileInfo(m_path).fileName();
}

QImage Document::render(int page, QSize pixelSize)
{
    if (page < 0 || pixelSize.isEmpty()) {
        return {};
    }
    QMutexLocker lock(&m_mutex);
    if (page >= m_sizes.size()) {
        return {}; // pages were removed meanwhile
    }
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
    bool haveText, haveAnnots, haveLinks;
    {
        QMutexLocker cache(&m_cacheMutex);
        haveText = m_text.contains(page);
        haveAnnots = m_annots.contains(page);
        haveLinks = m_links.contains(page);
    }
    if (!haveText) {
        auto text = std::make_shared<const PageText>(readText(p, m_sizes.at(page)));
        QMutexLocker cache(&m_cacheMutex);
        m_text.insert(page, std::move(text));
    }
    if (!haveAnnots) {
        refreshAnnotations(page, p);
    }
    if (!haveLinks) {
        QList<LinkData> links = readLinks(p, *m_doc);
        QMutexLocker cache(&m_cacheMutex);
        m_links.insert(page, std::move(links));
    }
}

void Document::refreshAnnotations(int page, const Poppler::Page &p)
{
    QList<AnnotationData> list;
    for (const std::unique_ptr<Poppler::Annotation> &ann : p.annotations(SupportedTypes)) {
        if (ann->flags() & Poppler::Annotation::Hidden) {
            continue;
        }
        AnnotationData d;
        d.id = annotationId(*ann);
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
        } else if (ann->subType() == Poppler::Annotation::AInk) {
            d.kind = AnnotationData::Kind::Ink;
            d.paths = static_cast<const Poppler::InkAnnotation *>(ann.get())->inkPaths();
            d.width = ann->style().width();
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

bool Document::cachedLinks(int page, QList<LinkData> *out) const
{
    QMutexLocker cache(&m_cacheMutex);
    const auto it = m_links.constFind(page);
    if (it == m_links.constEnd()) {
        return false;
    }
    *out = *it;
    return true;
}

QList<LinkData> Document::links(int page)
{
    QList<LinkData> list;
    if (cachedLinks(page, &list)) {
        return list;
    }
    QMutexLocker lock(&m_mutex);
    if (const std::unique_ptr<Poppler::Page> p = m_doc->page(page)) {
        primeCaches(page, *p);
    }
    lock.unlock();
    cachedLinks(page, &list);
    return list;
}

QList<QList<QRectF>> Document::search(int page, const QString &text, bool caseSensitive)
{
    QList<QList<QRectF>> matches;
    if (page < 0 || text.isEmpty()) {
        return matches;
    }
    QMutexLocker lock(&m_mutex);
    if (page >= m_sizes.size()) {
        return matches;
    }
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

void Document::removeAnnotations(const QList<QPair<int, QString>> &items)
{
    if (items.size() == 1) {
        removeAnnotation(items.first().first, items.first().second);
        return;
    }
    QList<AnnotationData> found;
    for (const auto &[page, id] : items) {
        for (const AnnotationData &d : annotations(page)) {
            if (d.id == id) {
                found.append(d);
                break;
            }
        }
    }
    if (found.isEmpty()) {
        return;
    }
    m_undo.beginMacro(tr("Delete %1 annotations").arg(found.size()));
    for (const AnnotationData &d : std::as_const(found)) {
        m_undo.push(new AnnotationCommand(this, d, {}, tr("Delete annotation")));
    }
    m_undo.endMacro();
}

void Document::updateAnnotations(const QList<AnnotationData> &list)
{
    if (list.size() == 1) {
        updateAnnotation(list.first());
        return;
    }
    QList<QPair<AnnotationData, AnnotationData>> changes;
    for (const AnnotationData &data : list) {
        for (const AnnotationData &d : annotations(data.page)) {
            if (d.id == data.id) {
                if (d != data) {
                    changes.append({d, data});
                }
                break;
            }
        }
    }
    if (changes.isEmpty()) {
        return;
    }
    m_undo.beginMacro(tr("Edit %1 annotations").arg(changes.size()));
    for (const auto &[before, after] : std::as_const(changes)) {
        m_undo.push(new AnnotationCommand(this, before, after, tr("Edit annotation")));
    }
    m_undo.endMacro();
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
            if (ann->uniqueName().isEmpty()) {
                ann->setUniqueName(data.id); // keep the id stable now its content changed
            }
        }
        refreshAnnotations(data.page, *p);
    }
    Q_EMIT pageChanged(data.page);
}

// --- pages --------------------------------------------------------------------

namespace {
// Copies `pages` (0-based, ascending) of `in` into a new file `out`, keeping
// their annotations, links and form fields, using qpdf.
bool copyPages(const QString &in, const QString &password, const QList<int> &pages, const QString &out, QString *error)
{
    QStringList ranges;
    for (int i = 0; i < pages.size();) {
        int j = i;
        while (j + 1 < pages.size() && pages.at(j + 1) == pages.at(j) + 1) {
            ++j;
        }
        ranges.append(i == j ? QString::number(pages.at(i) + 1) : QStringLiteral("%1-%2").arg(pages.at(i) + 1).arg(pages.at(j) + 1));
        i = j + 1;
    }
    std::vector<std::string> args{"qpdf", "--warning-exit-0", "--no-warn", in.toStdString()};
    if (!password.isEmpty()) {
        args.push_back("--password=" + password.toStdString());
    }
    args.insert(args.end(), {"--pages", ".", ranges.join(QLatin1Char(',')).toStdString(), "--", out.toStdString()});
    std::vector<const char *> argv;
    for (const std::string &a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    try {
        QPDFJob job;
        job.initializeFromArgv(argv.data());
        job.run();
        if (job.getExitCode() == QPDFJob::EXIT_ERROR) {
            *error = QCoreApplication::translate("Document", "The pages could not be copied.");
            return false;
        }
    } catch (const std::exception &e) {
        *error = QCoreApplication::translate("Document", "The pages could not be copied: %1").arg(QString::fromLocal8Bit(e.what()));
        return false;
    }
    return true;
}
} // namespace

class PagesCommand : public QUndoCommand
{
public:
    PagesCommand(Document *doc, const QString &before, const QString &after, const QString &text)
        : QUndoCommand(text)
        , m_doc(doc)
        , m_before(before)
        , m_after(after)
    {
    }

    void redo() override { m_doc->loadPages(m_after); }
    void undo() override { m_doc->loadPages(m_before); }

private:
    Document *m_doc;
    QString m_before;
    QString m_after;
};

std::optional<QList<int>> Document::parsePageRanges(const QString &text, int pageCount, QString *error)
{
    QSet<int> set;
    QString spec = text;
    spec.replace(QChar(0x2013), QLatin1Char('-')).replace(QChar(0x2014), QLatin1Char('-'));
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    static const QRegularExpression range(QStringLiteral("^(\\d*)\\s*-\\s*(\\d*)$"));
    // "3 - 5" is one range: glue the dashes to their numbers first.
    static const QRegularExpression spacedDash(QStringLiteral("\\s*-\\s*"));
    spec.replace(spacedDash, QStringLiteral("-"));
    const QStringList parts = spec.split(separators, Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        *error = tr("Enter pages, for example 1-3, 5, 8-.");
        return std::nullopt;
    }
    for (const QString &part : parts) {
        int from, to;
        bool ok = true;
        if (const QRegularExpressionMatch m = range.match(part); m.hasMatch()) {
            if (m.captured(1).isEmpty() && m.captured(2).isEmpty()) {
                ok = false;
            }
            from = m.captured(1).isEmpty() ? 1 : m.captured(1).toInt(&ok);
            to = ok ? (m.captured(2).isEmpty() ? pageCount : m.captured(2).toInt(&ok)) : 0;
        } else {
            from = to = part.toInt(&ok);
        }
        if (!ok) {
            *error = tr("“%1” is not a page or page range.").arg(part);
            return std::nullopt;
        }
        if (from > to) {
            std::swap(from, to);
        }
        if (from < 1 || to > pageCount) {
            *error = tr("Page %1 does not exist; the document has %n page(s).", nullptr, pageCount).arg(from < 1 ? from : to);
            return std::nullopt;
        }
        for (int i = from; i <= to; ++i) {
            set.insert(i - 1);
        }
    }
    QList<int> pages(set.cbegin(), set.cend());
    std::sort(pages.begin(), pages.end());
    return pages;
}

QString Document::formatPageRanges(const QList<int> &pages)
{
    QStringList parts;
    for (int i = 0; i < pages.size();) {
        int j = i;
        while (j + 1 < pages.size() && pages.at(j + 1) == pages.at(j) + 1) {
            ++j;
        }
        parts.append(i == j ? QString::number(pages.at(i) + 1) : QStringLiteral("%1-%2").arg(pages.at(i) + 1).arg(pages.at(j) + 1));
        i = j + 1;
    }
    return parts.join(QStringLiteral(", "));
}

bool Document::writeTo(const QString &path, QString *error)
{
    bool ok;
    {
        QMutexLocker lock(&m_mutex);
        const std::unique_ptr<Poppler::PDFConverter> converter = m_doc->pdfConverter();
        converter->setOutputFileName(path);
        converter->setPDFOptions(Poppler::PDFConverter::WithChanges);
        ok = converter->convert();
    }
    if (!ok) {
        *error = tr("The document could not be written.");
    }
    return ok;
}

QString Document::workFile()
{
    if (!m_work) {
        m_work = std::make_unique<QTemporaryDir>();
    }
    return m_work->isValid() ? m_work->filePath(QStringLiteral("%1.pdf").arg(++m_workCounter)) : QString();
}

bool Document::extractPages(const QList<int> &pages, const QString &path, QString *error)
{
    if (pages.isEmpty()) {
        *error = tr("No pages chosen.");
        return false;
    }
    const QString snapshot = workFile();
    if (snapshot.isEmpty()) {
        *error = tr("Cannot create a temporary file.");
        return false;
    }
    const QFileInfo info(path);
    QTemporaryFile temp(info.absolutePath() + QStringLiteral("/.") + info.fileName() + QStringLiteral(".XXXXXX"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        QFile::remove(snapshot);
        *error = tr("Cannot write to %1.").arg(info.absolutePath());
        return false;
    }
    const QString tempName = temp.fileName();
    temp.close();

    const bool ok = writeTo(snapshot, error) && copyPages(snapshot, m_password, pages, tempName, error);
    QFile::remove(snapshot);
    if (!ok) {
        QFile::remove(tempName);
        return false;
    }
    QFile::setPermissions(tempName, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    std::error_code ec;
    std::filesystem::rename(tempName.toStdString(), info.absoluteFilePath().toStdString(), ec);
    if (ec) {
        QFile::remove(tempName);
        *error = tr("Cannot write %1: %2").arg(path, QString::fromStdString(ec.message()));
        return false;
    }
    return true;
}

bool Document::removePages(const QList<int> &pages, QString *error)
{
    QList<int> keep;
    for (int i = 0; i < pageCount(); ++i) {
        if (!pages.contains(i)) {
            keep.append(i);
        }
    }
    if (keep.isEmpty()) {
        *error = tr("A document needs at least one page.");
        return false;
    }
    if (keep.size() == pageCount()) {
        *error = tr("No pages chosen.");
        return false;
    }
    const QString before = workFile(), after = workFile();
    if (before.isEmpty()) {
        *error = tr("Cannot create a temporary file.");
        return false;
    }
    if (!writeTo(before, error) || !copyPages(before, m_password, keep, after, error)) {
        QFile::remove(before);
        QFile::remove(after);
        return false;
    }
    const int n = pageCount() - keep.size();
    m_undo.push(new PagesCommand(this, before, after,
                                 n == 1 ? tr("Remove page %1").arg(pages.first() + 1) : tr("Remove %1 pages").arg(n)));
    return true;
}

bool Document::loadPages(const QString &path)
{
    const QByteArray pw = m_password.toUtf8();
    std::unique_ptr<Poppler::Document> doc = Poppler::Document::load(path, pw, pw);
    if (!doc || doc->isLocked() || doc->numPages() == 0) {
        qWarning("Cannot load %s", qPrintable(path));
        return false;
    }
    doc->setRenderHint(Poppler::Document::Antialiasing);
    doc->setRenderHint(Poppler::Document::TextAntialiasing);
    doc->setRenderHint(Poppler::Document::TextHinting, false);
    QList<QSizeF> sizes;
    for (int i = 0; i < doc->numPages(); ++i) {
        const std::unique_ptr<Poppler::Page> page = doc->page(i);
        QSizeF size = page ? page->pageSizeF() : QSizeF();
        if (size.width() < 1 || size.height() < 1) {
            size = QSizeF(595, 842);
        }
        sizes.append(size);
    }
    {
        QMutexLocker lock(&m_mutex);
        m_doc = std::move(doc);
        m_sizes = sizes;
        m_pageCount = sizes.size();
        QMutexLocker cache(&m_cacheMutex);
        m_text.clear();
        m_annots.clear();
        m_links.clear();
    }
    Q_EMIT pagesChanged();
    return true;
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

    if (!writeTo(tempName, error)) {
        QFile::remove(tempName);
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
