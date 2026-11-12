#include "startpage.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
const int kPadding = 8;

// Two lines per file: its name, and dimmed below it the folder it is in.
class RecentDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(100, 2 * option.fontMetrics.height() + 2 * kPadding);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget *widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const QFileInfo info(index.data(Qt::UserRole).toString());
        const QRect area = option.rect.adjusted(kPadding, kPadding, -kPadding, -kPadding);
        const int line = option.fontMetrics.height();
        const bool selected = option.state & QStyle::State_Selected;

        painter->save();
        painter->setFont(option.font);
        painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(QRect(area.left(), area.top(), area.width(), line), Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(info.fileName(), Qt::ElideMiddle, area.width()));
        if (!selected) {
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
        }
        painter->drawText(QRect(area.left(), area.top() + line, area.width(), line), Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(StartPage::displayPath(info.absolutePath()), Qt::ElideMiddle, area.width()));
        painter->restore();
    }
};
}

StartPage::StartPage(QWidget *parent)
    : QWidget(parent)
{
    auto *hint = new QLabel(tr("Open a PDF with Ctrl+O, or drop files here"), this);
    hint->setAlignment(Qt::AlignCenter);
    hint->setForegroundRole(QPalette::PlaceholderText);

    m_heading = new QLabel(tr("Recent files"), this);
    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    m_heading->setContentsMargins(kPadding, 0, 0, 0);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("recentList"));
    m_list->setItemDelegate(new RecentDelegate(m_list));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setMouseTracking(true); // hover feedback
    m_list->setCursor(Qt::PointingHandCursor);
    // Part of the page, not a box on it.
    m_list->viewport()->setAutoFillBackground(false);
    const auto open = [this](const QListWidgetItem *item) {
        Q_EMIT openRequested(item->data(Qt::UserRole).toString());
    };
    connect(m_list, &QListWidget::itemClicked, this, open);
    connect(m_list, &QListWidget::itemActivated, this, open); // Enter

    // A column in the middle of the window, however wide that is.
    auto *column = new QVBoxLayout;
    column->setSpacing(kPadding);
    column->addStretch(1);
    column->addWidget(hint);
    column->addSpacing(3 * kPadding);
    column->addWidget(m_heading);
    column->addWidget(m_list);
    column->addStretch(1);
    auto *row = new QHBoxLayout(this);
    row->addStretch(1);
    row->addLayout(column, 0);
    row->addStretch(1);
    m_list->setMinimumWidth(520);

    setRecentFiles({});
}

void StartPage::setRecentFiles(const QStringList &paths)
{
    m_list->clear();
    for (const QString &path : paths) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    // No taller than its rows, so the page stays centred around them.
    m_list->setMaximumHeight(paths.isEmpty() ? 0 : int(paths.size()) * m_list->sizeHintForRow(0));
    m_heading->setVisible(!paths.isEmpty());
    m_list->setVisible(!paths.isEmpty());
}

QString StartPage::displayPath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home) {
        return QStringLiteral("~");
    }
    if (path.startsWith(home + QLatin1Char('/'))) {
        return QLatin1Char('~') + path.mid(home.size());
    }
    return path;
}
