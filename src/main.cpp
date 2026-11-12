#include "mainwindow.h"
#include "singleinstance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QTimer>

#include <functional>

int main(int argc, char *argv[])
{
    // Read before Qt gets to it: the token is only good for one use.
    const QByteArray activationToken = qgetenv("XDG_ACTIVATION_TOKEN");

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
    const QCommandLineOption newWindowOption(QStringLiteral("new-window"),
                                             QStringLiteral("Open a window of its own, even if yap-dfe is already running."));
    parser.addOption(newWindowOption);
    // Testing aid: save a screenshot of the window once rendering settles, then exit.
    QCommandLineOption grabOption(QStringLiteral("grab"), QStringLiteral("Save a screenshot to <file> and exit."), QStringLiteral("file"));
    grabOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(grabOption);
    parser.process(app);

    SingleInstance::Request request;
    request.split = parser.isSet(splitOption);
    request.activationToken = activationToken;
    for (const QString &file : parser.positionalArguments()) {
        // The running window may have a different working directory.
        request.files << QDir::current().absoluteFilePath(file);
    }

    // Files go to the window that is already open, as tabs. If it does not
    // answer, this start carries on with a window of its own.
    SingleInstance instance(SingleInstance::defaultBasePath());
    const bool standalone = parser.isSet(newWindowOption) || parser.isSet(grabOption);
    if (!standalone && !instance.claim() && instance.send(request)) {
        return 0;
    }

    MainWindow window;
    if (request.files.isEmpty()) {
        // Started without files: pick up where the last window left off.
        window.restoreSession();
    } else {
        window.openRequest(request.files, request.split);
    }

    const std::function<void(const SingleInstance::Request &)> receive = [&](const SingleInstance::Request &incoming) {
        // Not underneath a dialog that is in the middle of something
        // (unsaved changes, a password): wait until it is gone.
        if (QApplication::activeModalWidget()) {
            QTimer::singleShot(250, &window, [&receive, incoming] { receive(incoming); });
            return;
        }
        window.openRequest(incoming.files, incoming.split);
        window.activate(incoming.activationToken);
    };
    QObject::connect(&instance, &SingleInstance::requestReceived, &window, receive);

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
