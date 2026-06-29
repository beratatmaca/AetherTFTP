#include "core/tftp_server.h"

#include "core/tftp_protocol.h"
#include "core/tftp_session.h"
#include "core/metrics_exporter.h"

#include <QDir>
#include <QFileInfo>
#include <QUdpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace tftp {

bool TftpServer::SubnetRule::matches(const QHostAddress &ip) const {
    return ip.isInSubnet(address, prefixLength);
}

TftpServer::TftpServer(QObject *parent) : QObject(parent) {}

TftpServer::~TftpServer() {
    close();
}

bool TftpServer::listen(const QHostAddress &address, quint16 port,
                        const QString &rootDir) {
    close();

    QDir dir(rootDir);
    if (!dir.exists()) {
        m_lastError =
            QStringLiteral("Root directory does not exist: %1").arg(rootDir);
        return false;
    }
    m_rootDir = dir.absolutePath();

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(address, port)) {
        m_lastError = m_socket->errorString();
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &TftpServer::onReadyRead);

    m_globalTimer.invalidate();

    logEvent(QStringLiteral("server_start"), QStringLiteral("server"),
             address.toString(), QString(), 0, QStringLiteral("success"),
             QStringLiteral("Listening on %1:%2, serving %3")
                 .arg(address.toString())
                 .arg(m_socket->localPort())
                 .arg(m_rootDir));
    return true;
}

void TftpServer::close() {
    stopMetricsServer();

    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_sessions.clear();
}

bool TftpServer::isListening() const {
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

quint16 TftpServer::port() const {
    return m_socket ? m_socket->localPort() : 0;
}

void TftpServer::setWhitelist(const QList<QString> &cidrs) {
    m_whitelist.clear();
    for (const QString &cidr : cidrs) {
        QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(cidr);
        if (parsed.first.isNull()) {
            QHostAddress ip(cidr);
            if (!ip.isNull()) {
                m_whitelist.append(
                    {ip, ip.protocol() == QAbstractSocket::IPv6Protocol ? 128
                                                                        : 32});
            }
        } else {
            m_whitelist.append({parsed.first, parsed.second});
        }
    }
}

void TftpServer::setBlacklist(const QList<QString> &cidrs) {
    m_blacklist.clear();
    for (const QString &cidr : cidrs) {
        QPair<QHostAddress, int> parsed = QHostAddress::parseSubnet(cidr);
        if (parsed.first.isNull()) {
            QHostAddress ip(cidr);
            if (!ip.isNull()) {
                m_blacklist.append(
                    {ip, ip.protocol() == QAbstractSocket::IPv6Protocol ? 128
                                                                        : 32});
            }
        } else {
            m_blacklist.append({parsed.first, parsed.second});
        }
    }
}

bool TftpServer::isAllowed(const QHostAddress &clientAddress,
                           bool isUpload) const {
    if (isUpload && m_readOnly) {
        return false;
    }

    for (const auto &rule : m_blacklist) {
        if (rule.matches(clientAddress)) {
            return false;
        }
    }

    if (!m_whitelist.isEmpty()) {
        bool match = false;
        for (const auto &rule : m_whitelist) {
            if (rule.matches(clientAddress)) {
                match = true;
                break;
            }
        }
        if (!match) {
            return false;
        }
    }

    return true;
}

QString TftpServer::resolveSafePath(const QString &filename) const {
    const QString canonicalRoot = QFileInfo(m_rootDir).canonicalFilePath();
    if (canonicalRoot.isEmpty())
        return QString();

    const QString cleaned = QDir::cleanPath(filename);
    if (cleaned.isEmpty() || QDir::isAbsolutePath(cleaned) ||
        cleaned.startsWith(QLatin1String(".."))) {
        return QString();
    }

    const QString candidate =
        QDir::cleanPath(canonicalRoot + QLatin1Char('/') + cleaned);
    QFileInfo info(candidate);
    QString canonicalCandidate;
    if (info.exists()) {
        canonicalCandidate = info.canonicalFilePath();
    } else {
        QFileInfo parentInfo(info.absolutePath());
        QString parentCanonical = parentInfo.canonicalFilePath();
        if (parentCanonical.isEmpty())
            return QString();
        canonicalCandidate = QDir::cleanPath(
            parentCanonical + QLatin1Char('/') + info.fileName());
    }

    if (canonicalCandidate.isEmpty())
        return QString();

    const QString rootPrefix = canonicalRoot + QLatin1Char('/');
    if (canonicalCandidate != canonicalRoot &&
        !canonicalCandidate.startsWith(rootPrefix))
        return QString();
    return canonicalCandidate;
}

void TftpServer::sendSessionPacket(const QByteArray &packet,
                                   const QHostAddress &address, quint16 port) {
    if (m_socket) {
        m_socket->writeDatagram(packet, address, port);
    }
}

void TftpServer::onReadyRead() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray buf(int(m_socket->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(buf.data(), buf.size(), &sender, &senderPort);

        // Single-port multiplexing routing using endpoint string. A session
        // registered in m_sessions has no socket of its own, so its datagrams
        // must always be routed here.
        const QString key =
            sender.toString() + QLatin1Char(':') + QString::number(senderPort);
        if (m_sessions.contains(key)) {
            m_sessions[key]->processDatagram(buf);
            continue;
        }

        Request req;
        if (!parseRequest(buf, req)) {
            // Not a request on the main socket — ignore (could be a stray).
            continue;
        }

        const bool isUpload = (req.op == OpCode::WRQ);
        const QString clientIp = sender.toString();

        // ACL Check
        if (!isAllowed(sender, isUpload)) {
            QByteArray err =
                buildError(ErrorCode::AccessViolation,
                           QStringLiteral("Access violation by ACL policy"));
            m_socket->writeDatagram(err, sender, senderPort);
            logEvent(QStringLiteral("transfer_rejected"), key, clientIp,
                     req.filename, 0, QStringLiteral("failure"),
                     QStringLiteral("Access violation by ACL policy"));
            continue;
        }

        const QString safePath = resolveSafePath(req.filename);
        if (safePath.isEmpty()) {
            QByteArray err = buildError(ErrorCode::AccessViolation,
                                        QStringLiteral("Access violation"));
            m_socket->writeDatagram(err, sender, senderPort);
            logEvent(QStringLiteral("transfer_rejected"), key, clientIp,
                     req.filename, 0, QStringLiteral("failure"),
                     QStringLiteral("Access violation"));
            continue;
        }

        auto *session =
            new TftpSession(sender, senderPort, req, safePath, this);
        session->setSessionLimit(m_sessionLimit);
        session->setSinglePortMode(m_singlePortMode);

        const QString fname = req.filename;

        connect(session, &TftpSession::retransmissionOccurred, this,
                [this]() { ++m_retransmissionCount; });

        connect(
            session, &TftpSession::finished, this,
            [this, session, fname, key, clientIp](bool ok, const QString &msg) {
                --m_activeSessions;
                m_historicalBytesTransferred += session->bytesTransferred();
                if (ok) {
                    ++m_transfersSuccess;
                } else {
                    ++m_transfersFailure;
                }
                // Unconditional: removes the routing entry if present (a
                // single-port session) and is a harmless no-op otherwise.
                // Guarding on the flag could strand a dangling pointer if the
                // mode was toggled off while the session was in flight.
                m_sessions.remove(key);
                emit transferFinished(fname, ok, msg);

                logEvent(
                    ok ? QStringLiteral("transfer_complete")
                       : QStringLiteral("transfer_error"),
                    key, clientIp, fname, session->blockCount(),
                    ok ? QStringLiteral("success") : QStringLiteral("failure"),
                    msg);

                session->deleteLater();
            });

        if (m_singlePortMode) {
            m_sessions.insert(key, session);
        }

        ++m_activeSessions;
        emit transferStarted(fname, isUpload);
        logEvent(QStringLiteral("transfer_start"), key, clientIp, fname, 0,
                 QStringLiteral("started"));
        session->start();
    }
}

qint64 TftpServer::requestGlobalDelay(qint64 packetSize) {
    if (m_globalLimit <= 0)
        return 0;

    const double packetSizeDouble = static_cast<double>(packetSize);

    if (!m_globalTimer.isValid()) {
        m_globalTimer.start();
        m_globalTokens = double(m_globalLimit);
    }
    qint64 elapsed = m_globalTimer.restart();
    m_globalTokens += (double(elapsed) / 1000.0) * double(m_globalLimit);
    double maxTokens = qMax<double>(65536.0 * 2.0, double(m_globalLimit));
    m_globalTokens = std::min(m_globalTokens, maxTokens);
    if (m_globalTokens >= packetSizeDouble) {
        m_globalTokens -= packetSizeDouble;
        return 0;
    } else {
        double needed = packetSizeDouble - m_globalTokens;
        qint64 delayMs =
            qint64(std::ceil(needed * 1000.0 / double(m_globalLimit)));
        m_globalTokens -= packetSizeDouble;
        return delayMs;
    }
}

void TftpServer::logEvent(const QString &eventType,
                          const QString &sessionIdentifier,
                          const QString &clientAddress, const QString &fileName,
                          int blockCount, const QString &status,
                          const QString &message) {
    if (m_jsonLoggingEnabled) {
        QJsonObject obj;
        obj.insert(QStringLiteral("timestamp"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        obj.insert(QStringLiteral("event_type"), eventType);
        obj.insert(QStringLiteral("session_id"), sessionIdentifier);
        obj.insert(QStringLiteral("client_ip"), clientAddress);
        obj.insert(QStringLiteral("file_name"), fileName);
        obj.insert(QStringLiteral("block_count"), blockCount);
        obj.insert(QStringLiteral("status"), status);
        if (!message.isEmpty()) {
            obj.insert(QStringLiteral("message"), message);
        }
        QJsonDocument doc(obj);
        emit logMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    } else {
        if (eventType == QLatin1String("server_start")) {
            emit logMessage(message);
        } else if (eventType == QLatin1String("transfer_rejected")) {
            if (message.contains(QLatin1String("ACL"))) {
                emit logMessage(QStringLiteral("Rejected by ACL: %1 from %2")
                                    .arg(fileName, clientAddress));
            } else {
                emit logMessage(
                    QStringLiteral("Rejected unsafe path: %1").arg(fileName));
            }
        } else if (eventType == QLatin1String("transfer_complete") ||
                   eventType == QLatin1String("transfer_error")) {
            emit logMessage(QStringLiteral("Transfer of %1 %2: %3")
                                .arg(fileName,
                                     status == QLatin1String("success")
                                         ? QStringLiteral("OK")
                                         : QStringLiteral("FAILED"),
                                     message));
        } else if (eventType == QLatin1String("transfer_start")) {
        } else {
            emit logMessage(
                QStringLiteral("[%1] %2 %3").arg(eventType, fileName, status));
        }
    }
}

qint64 TftpServer::totalBytesTransferred() const {
    auto activeSess = findChildren<TftpSession *>();
    qint64 bytes = m_historicalBytesTransferred;
    for (auto *session : activeSess) {
        bytes += session->bytesTransferred();
    }
    return bytes;
}

QString TftpServer::getMetricsFormatted() const {
    auto activeSess = findChildren<TftpSession *>();
    qint64 active = activeSess.size();
    qint64 bytes = totalBytesTransferred();

    QString result;
    QTextStream os(&result);

    os << "# HELP tftp_active_sessions Number of active TFTP sessions.\n";
    os << "# TYPE tftp_active_sessions gauge\n";
    os << "tftp_active_sessions " << active << "\n\n";

    os << "# HELP tftp_bytes_transferred_total Total bytes transferred.\n";
    os << "# TYPE tftp_bytes_transferred_total counter\n";
    os << "tftp_bytes_transferred_total " << bytes << "\n\n";

    os << "# HELP tftp_transfers_total Total number of TFTP transfers.\n";
    os << "# TYPE tftp_transfers_total counter\n";
    os << "tftp_transfers_total{status=\"success\"} " << m_transfersSuccess
       << "\n";
    os << "tftp_transfers_total{status=\"failure\"} " << m_transfersFailure
       << "\n\n";

    os << "# HELP tftp_retransmissions_total Total number of packet "
          "retransmissions.\n";
    os << "# TYPE tftp_retransmissions_total counter\n";
    os << "tftp_retransmissions_total " << m_retransmissionCount << "\n";

    return result;
}

bool TftpServer::startMetricsServer(quint16 port, const QHostAddress &address) {
    stopMetricsServer();
    m_metricsServer = new MetricsExporter(this, this);
    if (!m_metricsServer->listen(address, port)) {
        m_metricsServer->deleteLater();
        m_metricsServer = nullptr;
        return false;
    }
    return true;
}

void TftpServer::stopMetricsServer() {
    if (m_metricsServer) {
        m_metricsServer->close();
        m_metricsServer->deleteLater();
        m_metricsServer = nullptr;
    }
}

quint16 TftpServer::metricsServerPort() const {
    return m_metricsServer ? m_metricsServer->serverPort() : 0;
}

}  // namespace tftp
