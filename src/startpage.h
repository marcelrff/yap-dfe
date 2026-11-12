#pragma once

#include <QWidget>

class QLabel;
class QListWidget;

// Shown in place of the tabs while nothing is open: how to open a file, and
// the recently opened ones to pick from.
class StartPage : public QWidget
{
    Q_OBJECT

public:
    explicit StartPage(QWidget *parent = nullptr);

    void setRecentFiles(const QStringList &paths);

    // "~/papers" rather than "/home/someone/papers".
    static QString displayPath(const QString &path);

Q_SIGNALS:
    void openRequested(const QString &path);

private:
    QLabel *m_heading = nullptr;
    QListWidget *m_list = nullptr;
};
