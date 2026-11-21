#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QUndoStack>

#include <memory>

namespace Poppler {
class Document;
class Page;
}

// One annotation, as plain data. All geometry is normalised to the page:
// (0,0) is the top-left corner, (1,1) the bottom-right.
struct AnnotationData {
    enum class Kind { Highlight, Underline, StrikeOut, Squiggly, Note, Text, Ink };

    QString id; // unique within the document
    int page = 0;
    Kind kind = Kind::Highlight;
    QColor color; // marker / icon / text colour
    QRectF boundary;
    QList<QRectF> rects; // markup kinds: one rectangle per line of text
    QString contents; // the comment, or the text of a Text annotation
    double fontSize = 12; // Text only, in points
    QList<QList<QPointF>> paths; // Ink only: the pen strokes
    double width = 1; // Ink only: pen width in points

    bool isMarkup() const { return kind != Kind::Note && kind != Kind::Text && kind != Kind::Ink; }
    bool operator==(const AnnotationData &) const = default;
};

struct TextChar {
    QRectF rect; // normalised
    QString text;
    int word = 0;
    bool spaceAfter = false;
    bool breakAfter = false; // last character of a line
};
using PageText = QList<TextChar>;

// A clickable area on a page, as the PDF defines it.
struct LinkData {
    enum class Kind {
        Goto, // a place in this document
        Uri, // a web address or similar
        File, // a place in another PDF file
        Action, // a viewer command: first/previous/next/last page, back, forward
    };
    enum class Action { None, FirstPage, PreviousPage, NextPage, LastPage, Back, Forward };

    Kind kind = Kind::Goto;
    QRectF area; // normalised
    int page = -1; // Goto, File: target page, 0-based (-1: not given)
    double left = -1; // Goto: target position on that page, normalised;
    double top = -1; //       negative if the link leaves it open
    QString target; // Uri: the address; File: the file name as written in the PDF
    Action action = Action::None;
};

// A loaded PDF. One instance is shared by every view showing the same file.
// Lives on the GUI thread; render() may be called from any thread. Every
// annotation change goes through the undo stack, whose clean state is the
// document's "modified" flag.
class Document : public QObject
{
    Q_OBJECT

public:
    enum class LoadStatus { Ok, Locked, Failed };

    static std::shared_ptr<Document> load(const QString &path, const QString &password, LoadStatus *status);

    ~Document() override;

    QString path() const { return m_path; }
    QString fileName() const;
    int pageCount() const { return m_sizes.size(); }
    QSizeF pageSize(int page) const { return m_sizes.at(page); } // in points

    // Renders a whole page into an image of exactly pixelSize. Thread-safe.
    QImage render(int page, QSize pixelSize);

    // Text and annotations of a page. These load on demand and may wait for a
    // render in progress; the cached*() variants never block and return null
    // until the data is available (rendering a page also loads both).
    std::shared_ptr<const PageText> pageText(int page);
    std::shared_ptr<const PageText> cachedPageText(int page) const;
    QList<AnnotationData> annotations(int page);
    bool cachedAnnotations(int page, QList<AnnotationData> *out) const;
    QList<LinkData> links(int page);
    bool cachedLinks(int page, QList<LinkData> *out) const;

    // Where `text` occurs on a page. Each match is one normalised rectangle,
    // or two when it runs over a line end (hyphenated words). Thread-safe.
    QList<QList<QRectF>> search(int page, const QString &text, bool caseSensitive);

    // Undoable edits. addAnnotation() returns the new annotation's id.
    QString addAnnotation(AnnotationData data);
    void removeAnnotation(int page, const QString &id);
    void updateAnnotation(const AnnotationData &data);
    // Several at once, undone as one step. Items are (page, id).
    void removeAnnotations(const QList<QPair<int, QString>> &items);
    void updateAnnotations(const QList<AnnotationData> &list);

    QUndoStack *undoStack() { return &m_undo; }
    bool isModified() const { return !m_undo.isClean(); }

    // Writes the document including all changes. Saving to the current path
    // replaces the file atomically; another path makes that the document's path.
    bool save(const QString &path, QString *error);

    static QString textFontFamily();

Q_SIGNALS:
    void pageChanged(int page); // the page must be re-rendered
    void modifiedChanged(bool modified);
    void pathChanged();

private:
    friend class AnnotationCommand;
    Document();

    void applyAdd(const AnnotationData &data);
    void applyRemove(int page, const QString &id);
    void applyUpdate(const AnnotationData &data);
    void refreshAnnotations(int page, const Poppler::Page &p); // m_mutex held
    void primeCaches(int page, const Poppler::Page &p); // m_mutex held

    QString m_path;
    std::unique_ptr<Poppler::Document> m_doc;
    QList<QSizeF> m_sizes;
    QMutex m_mutex; // guards every use of m_doc

    mutable QMutex m_cacheMutex;
    QHash<int, std::shared_ptr<const PageText>> m_text;
    QHash<int, QList<AnnotationData>> m_annots;
    QHash<int, QList<LinkData>> m_links;

    QUndoStack m_undo;
};
