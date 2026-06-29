#include "core/tftp_session.h"
#include "core/tftp_server.h"

#include <QFile>
#include <QTimer>
#include <QUdpSocket>
#include <cmath>
#include <QThreadPool>
#include <QRunnable>

namespace tftp {

struct TftpSession::ReadTask : public QRunnable {
    QPointer<TftpSession> session;
    quint16 block;
    qint64 offset;
    int blockSize;

    ReadTask(TftpSession *s, quint16 b, qint64 o, int sz) : session(s), block(b), offset(o), blockSize(sz) { setAutoDelete(true); }

    void run() override {
        QByteArray payload;
        bool ok = false;
        auto s = session;
        auto b = block;
        auto o = offset;
        auto sz = blockSize;
        if (s) {
            if (s->m_file && s->m_file->seek(o)) {
                payload = s->m_file->read(sz);
                ok = true;
            }
        }
        if (s) {
            QMetaObject::invokeMethod(
                s.data(),
                [s, b, payload, ok]() {
                    if (s) {
                        s->onDataBlockRead(b, payload, ok);
                    }
                },
                Qt::QueuedConnection);
        }
    }
};

struct TftpSession::WriteTask : public QRunnable {
    QPointer<TftpSession> session;
    quint16 block;
    QByteArray payload;

    WriteTask(TftpSession *s, quint16 b, const QByteArray &p) : session(s), block(b), payload(p) { setAutoDelete(true); }

    void run() override {
        bool ok = false;
        auto s = session;
        auto b = block;
        auto p = payload;
        if (s) {
            if (s->m_file) {
                ok = (s->m_file->write(p) == p.size());
            }
        }
        if (s) {
            int size = p.size();
            QMetaObject::invokeMethod(
                s.data(),
                [s, b, size, ok]() {
                    if (s) {
                        s->onDataBlockWritten(b, size, ok);
                    }
                },
                Qt::QueuedConnection);
        }
    }
};

TftpSession::TftpSession(const QHostAddress &peer, quint16 peerPort, const Request &request, const QString &filePath, QObject *parent)
    : QObject(parent), m_peer(peer), m_peerPort(peerPort), m_request(request), m_filePath(filePath), m_isRead(request.op == OpCode::RRQ) {}

TftpSession::~TftpSession() = default;

bool TftpSession::start() {
    if (!m_singlePortMode) {
        m_socket = std::make_unique<QUdpSocket>(this);
        QHostAddress bindAddr = (m_peer.protocol() == QAbstractSocket::IPv6Protocol) ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4;
        if (!m_socket->bind(bindAddr, 0)) {
            finish(false, QStringLiteral("Failed to bind transfer socket"));
            return false;
        }
        connect(m_socket.get(), &QUdpSocket::readyRead, this, &TftpSession::onReadyRead);
    }

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &TftpSession::onTimeout);

    m_sendTimer = new QTimer(this);
    m_sendTimer->setSingleShot(true);
    connect(m_sendTimer, &QTimer::timeout, this, &TftpSession::onSendTimerTimeout);

    if (m_request.mode != QLatin1String(kModeOctet)) {
        sendError(ErrorCode::IllegalOperation, QStringLiteral("Only octet mode is supported"));
        return true;
    }

    m_file = std::make_unique<QFile>(m_filePath);

    if (m_isRead) {
        if (!m_file->open(QIODevice::ReadOnly)) {
            sendError(ErrorCode::FileNotFound, QStringLiteral("File not found"));
            return true;
        }
        m_totalBytes = m_file->size();
    } else {
        if (m_file->exists()) {
            sendError(ErrorCode::FileAlreadyExists, QStringLiteral("File already exists"));
            return true;
        }
        if (!m_file->open(QIODevice::WriteOnly)) {
            sendError(ErrorCode::AccessViolation, QStringLiteral("Cannot create file"));
            return true;
        }
    }

    const Options accepted = negotiateOptions(m_isRead ? m_totalBytes : -1);

    if (!accepted.isEmpty()) {
        m_oackPending = true;
        m_lastPacket = buildOack(accepted);
        sendPacketDeferred(m_lastPacket, true);
    } else if (m_isRead) {
        sendDataBlock(1);
    } else {
        m_currentBlock = 0;
        sendAck(0);
    }
    return true;
}

Options TftpSession::negotiateOptions(qint64 fileSize) {
    Options accepted;

    if (m_request.options.contains(QLatin1String(kOptBlksize))) {
        bool ok = false;
        int requested = m_request.options.value(QLatin1String(kOptBlksize)).toInt(&ok);
        if (ok) {
            m_blockSize = clampBlockSize(requested);
            accepted.insert(QLatin1String(kOptBlksize), QString::number(m_blockSize));
        }
    }

    if (m_request.options.contains(QLatin1String(kOptTimeout))) {
        bool ok = false;
        int secs = m_request.options.value(QLatin1String(kOptTimeout)).toInt(&ok);
        if (ok && secs >= 1 && secs <= 255) {
            m_timeoutMs = secs * 1000;
            accepted.insert(QLatin1String(kOptTimeout), QString::number(secs));
        }
    }

    if (m_request.options.contains(QLatin1String(kOptTsize))) {
        if (m_isRead && fileSize >= 0) {
            accepted.insert(QLatin1String(kOptTsize), QString::number(fileSize));
        } else if (!m_isRead) {
            bool ok = false;
            qint64 declared = m_request.options.value(QLatin1String(kOptTsize)).toLongLong(&ok);
            if (ok) {
                m_totalBytes = declared;
                accepted.insert(QLatin1String(kOptTsize), QString::number(declared));
            }
        }
    }

    return accepted;
}

void TftpSession::onReadyRead() {
    if (m_sendTimer && m_sendTimer->isActive()) {
        return;
    }

    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray buf(int(m_socket->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(buf.data(), buf.size(), &sender, &senderPort);

        if (senderPort != m_peerPort || sender != m_peer) {
            QByteArray stray = buildError(ErrorCode::UnknownTransferId, QStringLiteral("Unknown transfer ID"));
            m_socket->writeDatagram(stray, sender, senderPort);
            continue;
        }

        processDatagram(buf);
    }
}

void TftpSession::processDatagram(const QByteArray &buf) {
    if (m_sendTimer && m_sendTimer->isActive()) {
        return;
    }

    OpCode op;
    if (!peekOpCode(buf, op))
        return;

    if (op == OpCode::ERROR) {
        ErrorCode code;
        QString msg;
        parseError(buf, code, msg);
        finish(false, QStringLiteral("Peer error %1: %2").arg(int(code)).arg(msg));
        return;
    }

    if (m_isRead && op == OpCode::ACK) {
        quint16 block = 0;
        if (parseAck(buf, block))
            handleAck(block);
    } else if (!m_isRead && op == OpCode::DATA) {
        quint16 block = 0;
        QByteArray payload;
        if (parseData(buf, block, payload))
            handleData(block, payload);
    }
}

void TftpSession::sendPacketImmediate(const QByteArray &packet) {
    if (m_singlePortMode) {
        auto *server = qobject_cast<TftpServer *>(parent());
        if (server) {
            server->sendSessionPacket(packet, m_peer, m_peerPort);
        }
    } else {
        if (m_socket) {
            m_socket->writeDatagram(packet, m_peer, m_peerPort);
        }
    }
}

void TftpSession::sendPacketDeferred(const QByteArray &packet, bool armRetrans) {
    if (m_timer) {
        m_timer->stop();
    }

    qint64 delayMs = 0;
    qint64 packetSize = packet.size();

    qint64 sessionDelay = requestSessionDelay(packetSize);
    qint64 globalDelay = 0;
    if (auto *server = qobject_cast<TftpServer *>(parent())) {
        globalDelay = server->requestGlobalDelay(packetSize);
    }
    delayMs = qMax(sessionDelay, globalDelay);

    if (delayMs > 0 && m_sendTimer) {
        m_pendingSendPacket = packet;
        m_pendingArmRetransmit = armRetrans;
        m_sendTimer->start(int(delayMs));
    } else {
        sendPacketImmediate(packet);
        if (armRetrans) {
            armRetransmit();
        }
    }
}

void TftpSession::onSendTimerTimeout() {
    sendPacketImmediate(m_pendingSendPacket);
    if (m_pendingArmRetransmit) {
        armRetransmit();
    }
}

qint64 TftpSession::requestSessionDelay(qint64 packetSize) {
    if (m_sessionLimit <= 0)
        return 0;
    if (!m_sessionTokenTimer.isValid()) {
        m_sessionTokenTimer.start();
        m_sessionTokens = double(m_sessionLimit);
    }
    qint64 elapsed = m_sessionTokenTimer.restart();
    m_sessionTokens += (double(elapsed) / 1000.0) * m_sessionLimit;
    double maxTokens = qMax<double>(double(m_blockSize) * 2.0, double(m_sessionLimit));
    if (m_sessionTokens > maxTokens) {
        m_sessionTokens = maxTokens;
    }
    if (m_sessionTokens >= packetSize) {
        m_sessionTokens -= packetSize;
        return 0;
    } else {
        double needed = double(packetSize) - m_sessionTokens;
        qint64 delayMs = qint64(std::ceil(needed * 1000.0 / double(m_sessionLimit)));
        m_sessionTokens -= packetSize;
        return delayMs;
    }
}

// RRQ: we are sending the file

void TftpSession::sendDataBlock(quint16 block) {
    if (!m_file)
        return;

    const qint64 offset = qint64(block - 1) * m_blockSize;
    QThreadPool::globalInstance()->start(new ReadTask(this, block, offset, m_blockSize));
}

void TftpSession::onDataBlockRead(quint16 block, const QByteArray &payload, bool ok) {
    if (!ok || m_finished)
        return;

    m_currentBlock = block;
    m_oackPending = false;
    m_sentLastBlock = payload.size() < m_blockSize;
    m_lastPacket = buildData(block, payload);
    sendPacketDeferred(m_lastPacket, true);
}

void TftpSession::handleAck(quint16 block) {
    if (m_oackPending) {
        if (block == 0) {
            m_retries = 0;
            sendDataBlock(1);
        }
        return;
    }

    if (block != m_currentBlock)
        return;

    m_retries = 0;
    m_bytesTransferred = qMin<qint64>(qint64(block) * m_blockSize, m_totalBytes < 0 ? qint64(block) * m_blockSize : m_totalBytes);
    emit progress(m_bytesTransferred, m_totalBytes);

    if (m_sentLastBlock) {
        finish(true, QString());
        return;
    }
    sendDataBlock(block + 1);
}

// WRQ: we are receiving the file

void TftpSession::sendAck(quint16 block) {
    m_currentBlock = block;
    m_lastPacket = buildAck(block);
    sendPacketDeferred(m_lastPacket, !m_finished);
}

void TftpSession::handleData(quint16 block, const QByteArray &payload) {
    const quint16 expected = quint16(m_currentBlock + 1);

    if (block == m_currentBlock) {
        m_lastPacket = buildAck(block);
        sendPacketDeferred(m_lastPacket, false);
        return;
    }
    if (block != expected)
        return;

    if (m_oackPending)
        m_oackPending = false;

    QThreadPool::globalInstance()->start(new WriteTask(this, block, payload));
}

void TftpSession::onDataBlockWritten(quint16 block, int size, bool ok) {
    if (m_finished)
        return;

    if (!ok) {
        sendError(ErrorCode::DiskFull, QStringLiteral("Write failed"));
        return;
    }

    m_bytesTransferred += size;
    m_retries = 0;
    emit progress(m_bytesTransferred, m_totalBytes);

    const bool isLast = size < m_blockSize;
    if (isLast) {
        m_file->flush();
        m_finished = true;
        sendAck(block);
        finish(true, QString());
    } else {
        sendAck(block);
    }
}

// Timing and teardown

void TftpSession::armRetransmit() {
    if (m_timer)
        m_timer->start(m_timeoutMs);
}

void TftpSession::onTimeout() {
    if (m_finished)
        return;
    if (++m_retries > m_maxRetries) {
        finish(false, QStringLiteral("Transfer timed out"));
        return;
    }
    emit retransmissionOccurred();
    if (!m_lastPacket.isEmpty()) {
        sendPacketDeferred(m_lastPacket, true);
    }
}

void TftpSession::sendError(ErrorCode code, const QString &message) {
    sendPacketImmediate(buildError(code, message));
    finish(false, message);
}

void TftpSession::finish(bool ok, const QString &message) {
    if (m_emitted)
        return;
    m_emitted = true;
    m_finished = true;
    if (m_timer)
        m_timer->stop();
    if (m_sendTimer)
        m_sendTimer->stop();
    if (m_file && m_file->isOpen())
        m_file->close();
    emit finished(ok, message);
}

}  // namespace tftp
