#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("yap-dfe"));
    QApplication::setApplicationDisplayName(QStringLiteral("yap-dfe"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setDesktopFileName(QStringLiteral("yap-dfe"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Yet Another P-DF Editor: tabs, side-by-side panes, annotations"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("PDF files to open, one tab each."), QStringLiteral("[files...]"));
    const QCommandLineOption splitOption(QStringLiteral("split"),
                                         QStringLiteral("Start in split view: the last file opens in the right pane "
                                                        "(a single file is shown in both)."));
    parser.addOption(splitOption);
    // Testing aid: save a screenshot of the window once rendering settles, then exit.
    QCommandLineOption grabOption(QStringLiteral("grab"), QStringLiteral("Save a screenshot to <file> and exit."), QStringLiteral("file"));
    grabOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(grabOption);
    parser.process(app);

    MainWindow window;
    QStringList files = parser.positionalArguments();
    const bool split = parser.isSet(splitOption);
    if (split && files.size() > 1) {
        const QString right = files.takeLast();
        window.openFiles(files, 0);
        window.openFile(right, 1);
    } else {
        window.openFiles(files, 0);
        if (split) {
            window.toggleSplit();
        }
    }
    window.show();

    if (parser.isSet(grabOption)) {
        auto *poll = new QTimer(&window);
        auto *elapsed = new QElapsedTimer;
        elapsed->start();
        QObject::connect(poll, &QTimer::timeout, &window, [&, elapsed] {
            if ((elapsed->elapsed() > 300 && !window.isRendering()) || elapsed->elapsed() > 15000) {
                const bool ok = window.grab().save(parser.value(grabOption));
                QCoreApplication::exit(ok ? 0 : 1);
            }
        });
        poll->start(50);
    }

    return app.exec();
}
