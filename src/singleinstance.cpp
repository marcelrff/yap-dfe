#include "singleinstance.h"

#include <QDataStream>
#include <QDeadlineTimer>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

namespace {
const quint32 kMagic = 0x79617031; // "yap1"
}

SingleInstance::SingleInstance(const QString &basePath, QObject *parent)
    : QObject(parent)
    , m_socketPath(basePath + QStringLiteral(".sock"))
    , m_lock(std::make_unique<QLockFile>(basePath + QStringLiteral(".lock")))
{
    // The lock is held for as long as the process lives, never timed out.
    m_lock->setStaleLockTime(0);
}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::defaultBasePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    return dir + QStringLiteral("/yap-dfe");
}

bool SingleInstance::claim()
{
    // The lock, not the socket, decides who is first: a socket file can be
    // left behind by a crash, and two simultaneous launches must not both
    // conclude that nobody is listening.
    if (!m_lock->tryLock(0)) {
        return false;
    }
    QLocalServer::removeServer(m_socketPath);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketPath)) {
        delete m_server;
        m_server = nullptr;
        m_lock->unlock();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            // The sender writes one request and hangs up.
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                QDataStream in(socket->readAll());
                in.setVersion(QDataStream::Qt_6_5);
                quint32 magic = 0;
                Request request;
                in >> magic >> request.files >> request.split >> request.activationToken;
                socket->deleteLater();
                if (in.status() == QDataStream::Ok && magic == kMagic) {
                    Q_EMIT requestReceived(request);
                }
            });
        }
    });
    return true;
}

bool SingleInstance::send(const Request &request, int timeoutMs) const
{
    QByteArray message;
    QDataStream out(&message, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_5);
    out << kMagic << request.files << request.split << request.activationToken;

    // The other process may hold the lock but not be listening quite yet.
    const QDeadlineTimer deadline(timeoutMs);
    QLocalSocket socket;
    for (;;) {
        socket.connectToServer(m_socketPath, QIODevice::WriteOnly);
        if (socket.waitForConnected(int(qMax<qint64>(1, deadline.remainingTime())))) {
            break;
        }
        if (deadline.hasExpired()) {
            return false;
        }
        QThread::msleep(25);
    }
    socket.write(message);
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(int(qMax<qint64>(1, deadline.remainingTime())))) {
            return false;
        }
    }
    socket.disconnectFromServer();
    return true;
}
