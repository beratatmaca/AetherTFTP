#include "core/tftp_client.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QTimer>
#include <QUdpSocket>

namespace tftp {

TftpClient::TftpClient(QObject *parent) : QObject(parent) {}

TftpClient::~TftpClient() = default;

void TftpClient::downloadFile(const QString &host, quint16 port, const QString &remoteFile, const QString &localPath) {
    begin(Mode::Download, host, port, remoteFile, localPath);
}

void TftpClient::uploadFile(const QString &host, quint16 port, const QString &localPath, const QString &remoteFile) {
    begin(Mode::Upload, host, port, remoteFile, localPath);
}

bool TftpClient::begin(Mode mode, const QString &host, quint16 port, const QString &remoteFile, const QString &localPath) {
    if (m_running) {
        fail(QStringLiteral("A transfer is already in progress"));
        return false;
    }

    m_mode = mode;
    m_remoteFile = remoteFile;
    m_localPath = localPath;
    m_serverPort = port;
    m_tidLocked = false;
    m_blockSize = kDefaultBlockSize;
    m_windowSize = 1;
    m_retries = 0;
    m_block = 0;
    m_bytesTransferred = 0;
    m_totalBytes = -1;
    m_lastBlockSent = false;
    m_lastPacket.clear();

    m_isNetascii = (m_requestedMode == QLatin1String(kModeNetascii));
    m_netasciiData.clear();
    m_lastAckedBlock = 0;
    m_nextBlockToSend = 1;
    m_windowCache.clear();
    m_receiveBuffer.clear();
    m_lastBlockReceived = false;

    m_serverAddr = QHostAddress(host);
    if (m_serverAddr.isNull()) {
        QHostInfo info = QHostInfo::fromName(host);
        if (info.addresses().isEmpty()) {
            fail(QStringLiteral("Cannot resolve host: %1").arg(host));
            return false;
        }
        m_serverAddr = info.addresses().first();
    }

    if (localPath == QLatin1String("-")) {
        m_file = std::make_unique<QFile>();
        if (mode == Mode::Download) {
            if (!m_file->open(1, QIODevice::WriteOnly)) {
                fail(QStringLiteral("Cannot open stdout for writing"));
                return false;
            }
        } else {
            if (!m_file->open(0, QIODevice::ReadOnly)) {
                fail(QStringLiteral("Cannot open stdin for reading"));
                return false;
            }
            m_totalBytes = -1;
        }
    } else {
        m_file = std::make_unique<QFile>(localPath);
        if (mode == Mode::Download) {
            if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                fail(QStringLiteral("Cannot open local file for writing: %1").arg(localPath));
                return false;
            }
        } else {
            if (m_isNetascii) {
                if (!m_file->open(QIODevice::ReadOnly)) {
                    fail(QStringLiteral("Cannot open local file for reading: %1").arg(localPath));
                    return false;
                }
                m_netasciiData = toNetascii(m_file->readAll());
                m_file->close();
                m_totalBytes = m_netasciiData.size();
            } else {
                if (!m_file->open(QIODevice::ReadOnly)) {
                    fail(QStringLiteral("Cannot open local file for reading: %1").arg(localPath));
                    return false;
                }
                m_totalBytes = m_file->size();
            }
        }
    }

    m_socket = std::make_unique<QUdpSocket>(this);
    QHostAddress bindAddr = (m_serverAddr.protocol() == QAbstractSocket::IPv6Protocol) ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4;
    if (!m_socket->bind(bindAddr, 0)) {
        fail(QStringLiteral("Cannot bind client socket"));
        return false;
    }
    connect(m_socket.get(), &QUdpSocket::readyRead, this, &TftpClient::onReadyRead);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &TftpClient::onTimeout);

    m_running = true;
    sendInitialRequest();
    return true;
}

void TftpClient::sendInitialRequest() {
    Options opts;
    m_optionsRequested = false;
    if (m_requestedBlockSize != kDefaultBlockSize) {
        opts.insert(QLatin1String(kOptBlksize), QString::number(clampBlockSize(m_requestedBlockSize)));
        m_optionsRequested = true;
    }
    if (m_mode == Mode::Upload && m_totalBytes >= 0) {
        opts.insert(QLatin1String(kOptTsize), QString::number(m_totalBytes));
        m_optionsRequested = true;
    }
    const int timeoutSecs = m_timeoutMs / 1000;
    if (timeoutSecs >= 1 && timeoutSecs <= 255 && m_timeoutMs != 5000) {
        opts.insert(QLatin1String(kOptTimeout), QString::number(timeoutSecs));
        m_optionsRequested = true;
    }

    if (m_requestedWindowSize != 1) {
        opts.insert(QLatin1String(kOptWindowsize), QString::number(qBound(1, m_requestedWindowSize, 64)));
        m_optionsRequested = true;
    }

    const OpCode op = (m_mode == Mode::Download) ? OpCode::RRQ : OpCode::WRQ;
    m_lastPacket = buildRequest(op, m_remoteFile, m_requestedMode, opts);
    m_awaitingFirstReply = true;
    m_socket->writeDatagram(m_lastPacket, m_serverAddr, m_serverPort);
    armRetransmit();
}

void TftpClient::applyOack(const Options &options) {
    if (options.contains(QLatin1String(kOptBlksize))) {
        bool ok = false;
        int bs = options.value(QLatin1String(kOptBlksize)).toInt(&ok);
        if (ok)
            m_blockSize = clampBlockSize(bs);
    }
    if (options.contains(QLatin1String(kOptTsize)) && m_mode == Mode::Download) {
        bool ok = false;
        qint64 ts = options.value(QLatin1String(kOptTsize)).toLongLong(&ok);
        if (ok)
            m_totalBytes = ts;
    }
    if (options.contains(QLatin1String(kOptWindowsize))) {
        bool ok = false;
        int ws = options.value(QLatin1String(kOptWindowsize)).toInt(&ok);
        if (ok && ws >= 1 && ws <= 64)
            m_windowSize = ws;
    }
}

void TftpClient::onReadyRead() {
    while (m_running && m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray buf(int(m_socket->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(buf.data(), buf.size(), &sender, &senderPort);

        // Lock onto the server's transfer TID from the first reply (RFC 1350).
        if (!m_tidLocked) {
            if (sender.toIPv4Address() != m_serverAddr.toIPv4Address())
                continue;
            m_serverPort = senderPort;
            m_tidLocked = true;
        } else if (senderPort != m_serverPort) {
            // Stray datagram from an unknown TID — reply with an error, ignore.
            QByteArray err = buildError(ErrorCode::UnknownTransferId, QStringLiteral("Unknown transfer ID"));
            m_socket->writeDatagram(err, sender, senderPort);
            continue;
        }

        OpCode op;
        if (!peekOpCode(buf, op))
            continue;

        if (op == OpCode::ERROR) {
            ErrorCode code;
            QString msg;
            parseError(buf, code, msg);
            fail(QStringLiteral("Server error %1: %2").arg(int(code)).arg(msg));
            return;
        }

        if (op == OpCode::OACK) {
            Options oack;
            if (parseOack(buf, oack)) {
                applyOack(oack);
                m_awaitingFirstReply = false;
                m_retries = 0;
                if (m_mode == Mode::Download) {
                    m_block = 0;
                    m_lastPacket = buildAck(0);
                    m_socket->writeDatagram(m_lastPacket, m_serverAddr, m_serverPort);
                    armRetransmit();
                } else {
                    m_lastAckedBlock = 0;
                    m_nextBlockToSend = 1;
                    m_windowCache.clear();
                    fillWindow();
                }
            }
            continue;
        }

        if (m_mode == Mode::Download && op == OpCode::DATA) {
            quint16 block = 0;
            QByteArray payload;
            if (parseData(buf, block, payload))
                handleData(block, payload);
        } else if (m_mode == Mode::Upload && op == OpCode::ACK) {
            quint16 block = 0;
            if (parseAck(buf, block))
                handleAck(block);
        }
    }
}

// Download logic

void TftpClient::handleData(quint16 block, const QByteArray &payload) {
    m_awaitingFirstReply = false;

    qint64 expectedStart = m_block + 1;
    qint64 expectedEnd = m_block + m_windowSize;
    qint64 block64 = -1;
    for (qint64 b = expectedStart; b <= expectedEnd; ++b) {
        if (quint16(b) == block) {
            block64 = b;
            break;
        }
    }

    if (block64 == -1) {
        m_socket->writeDatagram(buildAck(quint16(m_block)), m_serverAddr, m_serverPort);
        return;
    }

    // A legitimate new block arrived, so this exchange succeeded — reset the
    // retry budget. Without this, m_retries accumulates across the whole
    // download (it's only ever reset by begin()/OACK), so occasional
    // transient packet loss on unrelated earlier blocks eats into the retry
    // budget for later blocks and can abort an otherwise-healthy transfer.
    m_retries = 0;

    QByteArray finalPayload = payload;
    if (!m_pskKey.isEmpty()) {
        finalPayload = cryptPayload(payload, m_pskKey, block);
    }

    m_receiveBuffer[block64] = finalPayload;

    while (m_receiveBuffer.contains(m_block + 1)) {
        qint64 nextBlock = m_block + 1;
        QByteArray nextPayload = m_receiveBuffer.take(nextBlock);
        if (m_isNetascii) {
            m_netasciiData.append(nextPayload);
        } else {
            if (m_file->write(nextPayload) != nextPayload.size()) {
                fail(QStringLiteral("Local write failed"));
                return;
            }
        }
        m_block = nextBlock;
        m_bytesTransferred += nextPayload.size();
        emit progress(m_bytesTransferred, m_totalBytes);
        if (nextPayload.size() < m_blockSize) {
            m_lastBlockReceived = true;
        }
    }

    m_lastPacket = buildAck(quint16(m_block));
    m_socket->writeDatagram(m_lastPacket, m_serverAddr, m_serverPort);

    if (m_lastBlockReceived) {
        if (!m_isNetascii) {
            m_file->flush();
        }
        succeed();
    } else {
        armRetransmit();
    }
}

// Upload logic

void TftpClient::sendDataBlock(qint64 block) {
    QByteArray payload;
    if (m_isNetascii) {
        const qint64 offset = (block - 1) * m_blockSize;
        payload = m_netasciiData.mid(offset, m_blockSize);
    } else {
        if (!m_file->isSequential()) {
            const qint64 offset = (block - 1) * m_blockSize;
            m_file->seek(offset);
        }
        payload = m_file->read(m_blockSize);
    }

    m_lastBlockSent = payload.size() < m_blockSize;
    QByteArray finalPayload = payload;
    if (!m_pskKey.isEmpty()) {
        finalPayload = cryptPayload(payload, m_pskKey, quint16(block));
    }
    QByteArray packet = buildData(quint16(block), finalPayload);
    m_windowCache.insert(block, packet);
    m_socket->writeDatagram(packet, m_serverAddr, m_serverPort);
    armRetransmit();
}

void TftpClient::fillWindow() {
    while (m_nextBlockToSend <= m_lastAckedBlock + m_windowSize && !m_lastBlockSent) {
        sendDataBlock(m_nextBlockToSend);
        m_nextBlockToSend++;
    }
}

void TftpClient::handleAck(quint16 block) {
    m_awaitingFirstReply = false;

    if (block == 0 && m_lastAckedBlock == 0 && m_nextBlockToSend == 1 && !m_optionsRequested) {
        m_retries = 0;
        m_windowCache.clear();
        fillWindow();
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

        if (m_lastBlockSent && m_windowCache.isEmpty()) {
            succeed();
            return;
        }
        fillWindow();
    }
}

// Timing and teardown

void TftpClient::armRetransmit() {
    if (m_timer)
        m_timer->start(m_timeoutMs);
}

void TftpClient::onTimeout() {
    if (!m_running)
        return;
    if (++m_retries > m_maxRetries) {
        fail(QStringLiteral("Transfer timed out"));
        return;
    }
    if (m_mode == Mode::Upload) {
        for (auto it = m_windowCache.constBegin(); it != m_windowCache.constEnd(); ++it) {
            m_socket->writeDatagram(it.value(), m_serverAddr, m_serverPort);
        }
        armRetransmit();
    } else {
        if (!m_lastPacket.isEmpty()) {
            m_socket->writeDatagram(m_lastPacket, m_serverAddr, m_serverPort);
            armRetransmit();
        }
    }
}

void TftpClient::abort() {
    if (!m_running)
        return;

    if (m_socket && m_tidLocked) {
        const QByteArray err = buildError(ErrorCode::NotDefined, QStringLiteral("Transfer cancelled by user"));
        m_socket->writeDatagram(err, m_serverAddr, m_serverPort);
    }

    m_running = false;
    if (m_timer)
        m_timer->stop();

    const bool wasDownload = (m_mode == Mode::Download);
    if (m_file && m_file->isOpen())
        m_file->close();
    if (wasDownload && !m_localPath.isEmpty())
        QFile::remove(m_localPath);

    emit transferFinished(false);
}

void TftpClient::fail(const QString &message) {
    if (!m_running && m_mode == Mode::Idle) {
        emit errorOccurred(message);
        emit transferFinished(false);
        return;
    }
    m_running = false;
    if (m_timer)
        m_timer->stop();
    if (m_file && m_file->isOpen())
        m_file->close();
    emit errorOccurred(message);
    emit transferFinished(false);
}

void TftpClient::succeed() {
    m_running = false;
    if (m_timer)
        m_timer->stop();
    if (m_mode == Mode::Download && m_isNetascii && m_file && m_file->isOpen()) {
        m_file->resize(0);
        m_file->write(fromNetascii(m_netasciiData));
    }
    if (m_file && m_file->isOpen())
        m_file->close();
    emit transferFinished(true);
}

}  // namespace tftp
