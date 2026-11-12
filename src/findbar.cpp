#include "findbar.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

FindBar::FindBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);

    auto *closeButton = new QToolButton(this);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    closeButton->setAutoRaise(true);
    closeButton->setToolTip(tr("Close (Esc)"));
    layout->addWidget(closeButton);

    layout->addWidget(new QLabel(tr("Find:"), this));
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("findEdit"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setMinimumWidth(260);
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit);

    auto *previous = new QToolButton(this);
    previous->setArrowType(Qt::UpArrow);
    previous->setAutoRaise(true);
    previous->setToolTip(tr("Previous match (Shift+Enter)"));
    layout->addWidget(previous);
    auto *next = new QToolButton(this);
    next->setArrowType(Qt::DownArrow);
    next->setAutoRaise(true);
    next->setToolTip(tr("Next match (Enter)"));
    layout->addWidget(next);

    m_case = new QCheckBox(tr("Match case"), this);
    layout->addWidget(m_case);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("findStatus"));
    layout->addWidget(m_status);
    layout->addStretch();

    // Searching a long document on every keystroke would only produce
    // results that are thrown away.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150);
    connect(m_debounce, &QTimer::timeout, this, &FindBar::queryChanged);
    connect(m_edit, &QLineEdit::textChanged, m_debounce, qOverload<>(&QTimer::start));
    connect(m_case, &QCheckBox::toggled, this, &FindBar::queryChanged);
    connect(next, &QToolButton::clicked, this, &FindBar::findNext);
    connect(previous, &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(closeButton, &QToolButton::clicked, this, &FindBar::close);

    hide();
}

QString FindBar::text() const
{
    return m_edit->text();
}

bool FindBar::caseSensitive() const
{
    return m_case->isChecked();
}

void FindBar::open(const QString &text)
{
    show();
    if (!text.isEmpty() && text != m_edit->text()) {
        m_edit->setText(text);
    }
    m_edit->setFocus();
    m_edit->selectAll();
    // Re-opening with the old query brings its matches back.
    m_debounce->stop();
    Q_EMIT queryChanged();
}

void FindBar::setStatus(const QString &status, bool noMatches)
{
    m_status->setText(status);
    QPalette p = palette();
    if (noMatches) {
        p.setColor(QPalette::Text, QColor(220, 60, 60));
    }
    m_edit->setPalette(p);
}

void FindBar::close()
{
    m_debounce->stop();
    hide();
    Q_EMIT closed();
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            // Search for what is in the box right now, not what was there
            // when the debounce timer last fired.
            if (m_debounce->isActive()) {
                m_debounce->stop();
                Q_EMIT queryChanged();
            }
            if (key->modifiers() & Qt::ShiftModifier) {
                Q_EMIT findPrevious();
            } else {
                Q_EMIT findNext();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
