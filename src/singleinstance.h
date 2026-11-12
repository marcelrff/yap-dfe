#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <memory>

class QLocalServer;
class QLockFile;

// Lets a second launch hand its files to the window that is already running,
// so they open as tabs there instead of in a window of their own.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    struct Request {
        QStringList files; // absolute paths
        bool split = false;
        // Wayland only lets a window raise itself with a token from whoever
        // caused the launch; the launcher puts it in XDG_ACTIVATION_TOKEN.
        QByteArray activationToken;
    };

    // `basePath` names the lock and socket files: <basePath>.lock, <basePath>.sock.
    explicit SingleInstance(const QString &basePath, QObject *parent = nullptr);
    ~SingleInstance() override;

    // Per user and login session.
    static QString defaultBasePath();

    // Makes this process the one that receives requests. False if another
    // process already is.
    bool claim();

    // Hands a request to the process that claimed. False if none took it.
    bool send(const Request &request, int timeoutMs = 3000) const;

Q_SIGNALS:
    void requestReceived(const SingleInstance::Request &request);

private:
    QString m_socketPath;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
};
