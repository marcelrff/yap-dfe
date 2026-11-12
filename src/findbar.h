#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTimer;

// The search strip under the pages. It only collects input; the main window
// passes it on to the current view and feeds the match count back.
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget *parent = nullptr);

    QString text() const;
    bool caseSensitive() const;
    // Shows the bar and puts the cursor in it; non-empty `text` replaces the query.
    void open(const QString &text);
    void setStatus(const QString &status, bool noMatches);

Q_SIGNALS:
    void queryChanged(); // text or case option changed (typing is debounced)
    void findNext();
    void findPrevious();
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void close();

    QLineEdit *m_edit;
    QCheckBox *m_case;
    QLabel *m_status;
    QTimer *m_debounce;
};
