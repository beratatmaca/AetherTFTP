#include "core/tftp_session.h"
#include "core/tftp_server.h"

#include <QFile>
#include <QTimer>
#include <QUdpSocket>
#include <cmath>
#include <utility>
#include <QThreadPool>
#include <QRunnable>

namespace tftp {

struct TftpSession::ReadTask : public QRunnable {
    const QPointer<TftpSession> session;
    const QString filePath;
    const qint64 block;
    const qint64 offset;
    const int blockSize;
    const bool isNetascii;
    const QByteArray netasciiData;  // shallow copy — QByteArray is implicitly shared

    ReadTask(TftpSession *s, qint64 b, qint64 o, int sz)
        : session(s),
          filePath(s->m_filePath),
          block(b),
          offset(o),
          blockSize(sz),
          isNetascii(s->m_isNetascii),
          netasciiData(s->m_isNetascii ? s->m_netasciiData : QByteArray()) {
        setAutoDelete(true);
    }

    void run() override {
        QByteArray payload;
        bool ok = false;
        auto s = session;
        auto b = block;
        if (isNetascii) {
            // netasciiData is an implicitly-shared QByteArray; mid() is safe from any thread.
            payload = netasciiData.mid(offset, blockSize);
            ok = true;
        } else {
            // Open a private file descriptor per task to avoid seek/read races
            // when multiple ReadTasks run concurrently for window sizes > 1.
            QFile f(filePath);
            if (f.open(QIODevice::ReadOnly) && f.seek(offset)) {
                payload = f.read(blockSize);
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
    const QPointer<TftpSession> session;
    const qint64 block;
    const QByteArray payload;
    const bool isNetascii;

    WriteTask(TftpSession *s, qint64 b, QByteArray p)
        : session(s), block(b), payload(std::move(p)), isNetascii(s ? s->m_isNetascii : false) {
        setAutoDelete(true);
    }

    void run() override {
        bool ok = false;
        auto s = session;
        auto b = block;
        auto p = payload;
        if (isNetascii) {
            ok = true;
        } else if (s && s->m_file) {
            ok = (s->m_file->write(p) == p.size());
        }
        if (s) {
            int size = p.size();
            QMetaObject::invokeMethod(
                s.data(),
                [s, b, p, size, ok, netascii = isNetascii]() {
                    if (s) {
                        if (netascii && ok) {
                            s->m_netasciiData.append(p);
                        }
                        s->onDataBlockWritten(b, size, ok);
                    }
                },
                Qt::QueuedConnection);
        }
    }
};

TftpSession::TftpSession(const QHostAddress &peer, quint16 peerPort, const Request &request, QString filePath, QObject *parent)
    : QObject(parent),
      m_peer(peer),
      m_peerPort(peerPort),
      m_request(request),
      m_filePath(std::move(filePath)),
      m_isRead(request.op == OpCode::RRQ) {}

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

    if (m_request.mode == QLatin1String(kModeNetascii)) {
        m_isNetascii = true;
    } else if (m_request.mode != QLatin1String(kModeOctet)) {
        sendError(ErrorCode::IllegalOperation, QStringLiteral("Only octet and netascii modes are supported"));
        return true;
    }

    m_file = std::make_unique<QFile>(m_filePath);

    if (m_isRead) {
        if (!m_file->open(QIODevice::ReadOnly)) {
            sendError(ErrorCode::FileNotFound, QStringLiteral("File not found"));
            return true;
        }
        if (m_isNetascii) {
            m_netasciiData = toNetascii(m_file->readAll());
            m_file->close();
            m_totalBytes = m_netasciiData.size();
        } else {
            m_totalBytes = m_file->size();
        }
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
        m_oackPending = false;
        m_lastAckedBlock = 0;
        m_nextBlockToSend = 1;
        m_windowCache.clear();
        fillWindow();
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

    if (m_request.options.contains(QLatin1String(kOptWindowsize))) {
        bool ok = false;
        int requested = m_request.options.value(QLatin1String(kOptWindowsize)).toInt(&ok);
        if (ok && requested >= 1) {
            m_windowSize = qMin(requested, 64);
            accepted.insert(QLatin1String(kOptWindowsize), QString::number(m_windowSize));
        }
    }

    return accepted;
}

void TftpSession::onReadyRead() {
    if (m_sendTimer && m_sendTimer->isActive()) {
        return;
    }

    while (!m_finished && m_socket && m_socket->hasPendingDatagrams()) {
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
    if (m_finished) {
        return;
    }
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
    }
    double needed = double(packetSize) - m_sessionTokens;
    auto delayMs = qint64(std::ceil(needed * 1000.0 / double(m_sessionLimit)));
    m_sessionTokens -= packetSize;
    return delayMs;
}

// RRQ: we are sending the file

void TftpSession::sendDataBlock(qint64 block) {
    if (!m_file && !m_isNetascii)
        return;

    const qint64 offset = (block - 1) * m_blockSize;
    QThreadPool::globalInstance()->start(new ReadTask(this, block, offset, m_blockSize));
}

void TftpSession::fillWindow() {
    while (m_nextBlockToSend <= m_lastAckedBlock + m_windowSize && !m_sentLastBlock && m_readTasksActive < m_windowSize) {
        m_readTasksActive++;
        sendDataBlock(m_nextBlockToSend);
        m_nextBlockToSend++;
    }
}

void TftpSession::onDataBlockRead(qint64 block, const QByteArray &payload, bool ok) {
    m_readTasksActive--;
    if (!ok || m_finished)
        return;

    m_readBuffer[block] = payload;

    while (m_readBuffer.contains(m_currentBlock + 1)) {
        qint64 nextBlock = m_currentBlock + 1;
        QByteArray nextPayload = m_readBuffer.take(nextBlock);
        m_currentBlock = nextBlock;
        m_sentLastBlock = nextPayload.size() < m_blockSize;
        QByteArray finalPayload = nextPayload;
        if (!m_pskKey.isEmpty()) {
            finalPayload = cryptPayload(nextPayload, m_pskKey, quint16(nextBlock));
        }
        m_lastPacket = buildData(quint16(nextBlock), finalPayload);
        m_windowCache.insert(nextBlock, m_lastPacket);
        sendPacketDeferred(m_lastPacket, true);
    }

    fillWindow();
}

void TftpSession::handleAck(quint16 block) {
    if (m_oackPending) {
        if (block == 0) {
            m_retries = 0;
            m_oackPending = false;
            m_lastAckedBlock = 0;
            m_nextBlockToSend = 1;
            m_windowCache.clear();
            fillWindow();
        }
        return;
    }

    qint64 acked64 = -1;
    for (auto it = m_windowCache.constBegin(); it != m_windowCache.constEnd(); ++it) {
        if (quint16(it.key()) == block) {
            acked64 = it.key();
            break;
        }
    }

    if (acked64 > m_lastAckedBlock) {
        m_retries = 0;
        for (qint64 b = m_lastAckedBlock + 1; b <= acked64; ++b) {
            m_windowCache.remove(b);
        }
        m_lastAckedBlock = acked64;
        m_bytesTransferred = qMin<qint64>(m_lastAckedBlock * m_blockSize, m_totalBytes < 0 ? m_lastAckedBlock * m_blockSize : m_totalBytes);
        emit progress(m_bytesTransferred, m_totalBytes);

        if (m_sentLastBlock && m_windowCache.isEmpty()) {
            finish(true, QString());
            return;
        }
        fillWindow();
    }
}

// WRQ: we are receiving the file

void TftpSession::sendAck(qint64 block) {
    m_currentBlock = block;
    m_lastPacket = buildAck(quint16(block));
    sendPacketDeferred(m_lastPacket, !m_finished);
}

void TftpSession::handleData(quint16 block, const QByteArray &payload) {
    if (m_oackPending)
        m_oackPending = false;

    qint64 expectedStart = m_currentBlock + 1;
    qint64 expectedEnd = m_currentBlock + m_windowSize;
    qint64 block64 = -1;
    for (qint64 b = expectedStart; b <= expectedEnd; ++b) {
        if (quint16(b) == block) {
            block64 = b;
            break;
        }
    }

    if (block64 == -1) {
        sendAck(m_currentBlock);
        return;
    }

    QByteArray finalPayload = payload;
    if (!m_pskKey.isEmpty()) {
        finalPayload = cryptPayload(payload, m_pskKey, block);
    }

    m_receiveBuffer[block64] = finalPayload;

    if (!m_writeActive && m_receiveBuffer.contains(m_currentBlock + 1)) {
        m_writeActive = true;
        QThreadPool::globalInstance()->start(new WriteTask(this, m_currentBlock + 1, m_receiveBuffer.take(m_currentBlock + 1)));
    }
}

void TftpSession::onDataBlockWritten(qint64 block, int size, bool ok) {
    m_writeActive = false;
    if (m_finished)
        return;

    if (!ok) {
        sendError(ErrorCode::DiskFull, QStringLiteral("Write failed"));
        return;
    }

    m_bytesTransferred += size;
    m_retries = 0;
    m_currentBlock = block;
    emit progress(m_bytesTransferred, m_totalBytes);

    const bool isLast = size < m_blockSize;
    if (isLast) {
        if (m_file && m_file->isOpen()) {
            m_file->flush();
        }
        m_finished = true;
        sendAck(block);
        finish(true, QString());
    } else {
        sendAck(block);
        if (m_receiveBuffer.contains(m_currentBlock + 1)) {
            m_writeActive = true;
            QThreadPool::globalInstance()->start(new WriteTask(this, m_currentBlock + 1, m_receiveBuffer.take(m_currentBlock + 1)));
        }
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
    if (m_isRead) {
        for (auto it = m_windowCache.constBegin(); it != m_windowCache.constEnd(); ++it) {
            sendPacketDeferred(it.value(), true);
        }
    } else {
        if (!m_lastPacket.isEmpty()) {
            sendPacketDeferred(m_lastPacket, true);
        }
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
    if (ok && !m_isRead && m_isNetascii && m_file && m_file->isOpen()) {
        m_file->resize(0);
        m_file->write(fromNetascii(m_netasciiData));
    }
    if (m_file && m_file->isOpen())
        m_file->close();
    emit finished(ok, message);
}

}  // namespace tftp
