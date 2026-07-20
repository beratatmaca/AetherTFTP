#include "core/pxe_proxy_dhcp.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QtEndian>

namespace tftp {

namespace {

constexpr quint32 kDhcpMagicCookie = 0x63825363;

struct DhcpHeader {
    quint8 op;
    quint8 htype;
    quint8 hlen;
    quint8 hops;
    quint32 xid;
    quint16 secs;
    quint16 flags;
    quint32 ciaddr;
    quint32 yiaddr;
    quint32 siaddr;
    quint32 giaddr;
    quint8 chaddr[16];
    char sname[64];
    char file[128];
    quint32 magicCookie;
};

QString formatMacAddress(const quint8 *mac, int len) {
    QStringList parts;
    for (int i = 0; i < len; ++i) {
        parts << QStringLiteral("%1").arg(mac[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return parts.join(QLatin1Char(':'));
}

}  // namespace

PxeProxyDhcp::PxeProxyDhcp(QObject *parent) : QObject(parent) {}

PxeProxyDhcp::~PxeProxyDhcp() {
    close();
}

bool PxeProxyDhcp::listen(quint16 port, const QHostAddress &bindAddr) {
    close();

    m_socket = new QUdpSocket(this);
    // Bind with ReuseAddressHint and ShareAddress to allow running alongside standard DHCP services
    if (!m_socket->bind(bindAddr, port, QUdpSocket::ReuseAddressHint | QUdpSocket::ShareAddress)) {
        emit logMessage(QStringLiteral("ProxyDHCP failed to bind to UDP port %1: %2").arg(port).arg(m_socket->errorString()));
        delete m_socket;
        m_socket = nullptr;
        return false;
    }

    connect(m_socket, &QUdpSocket::readyRead, this, &PxeProxyDhcp::onReadyRead);
    emit logMessage(QStringLiteral("ProxyDHCP listening on %1:%2 (Bootfile: %3)").arg(bindAddr.toString()).arg(port).arg(m_bootFile));
    return true;
}

void PxeProxyDhcp::close() {
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

bool PxeProxyDhcp::isListening() const {
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

quint16 PxeProxyDhcp::port() const {
    return m_socket ? m_socket->localPort() : 0;
}

void PxeProxyDhcp::onReadyRead() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray datagram(int(m_socket->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;

        qint64 bytesRead = m_socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        if (bytesRead < static_cast<qint64>(sizeof(DhcpHeader))) {
            continue;
        }

        // Determine effective server address to advertise
        QHostAddress serverAddr = m_tftpServerAddr;
        if (serverAddr.isNull() || serverAddr.isLoopback()) {
            serverAddr = m_socket->localAddress();
            if (serverAddr.isNull() || serverAddr == QHostAddress::Any || serverAddr == QHostAddress::AnyIPv4) {
                // Find first non-loopback IPv4 address
                const QList<QHostAddress> addrs = QNetworkInterface::allAddresses();
                for (const auto &addr : addrs) {
                    if (addr.protocol() == QAbstractSocket::IPv6Protocol || addr.isLoopback()) {
                        continue;
                    }
                    serverAddr = addr;
                    break;
                }
            }
        }

        QString clientMac;
        QByteArray response = processDhcpPacket(datagram, serverAddr, m_bootFile, m_tftpServerAddr, m_tftpServerName, &clientMac);
        if (response.isEmpty()) {
            continue;
        }

        // Send ProxyDHCP response via broadcast or directly to client/relay
        QHostAddress targetAddr = QHostAddress::Broadcast;
        quint16 targetPort = (senderPort != 0) ? senderPort : 68;

        m_socket->writeDatagram(response, targetAddr, targetPort);
        emit requestHandled(clientMac, sender, m_bootFile);
        emit logMessage(
            QStringLiteral("ProxyDHCP offered bootfile '%1' to PXE client %2 (%3)").arg(m_bootFile, clientMac, sender.toString()));
    }
}

QByteArray PxeProxyDhcp::processDhcpPacket(const QByteArray &requestData, const QHostAddress &serverAddr, const QString &bootFile,
                                           const QHostAddress &tftpServerAddr, const QString &tftpServerName, QString *outClientMac) {
    if (requestData.size() < static_cast<int>(sizeof(DhcpHeader))) {
        return {};
    }

    const auto *reqHeader = reinterpret_cast<const DhcpHeader *>(requestData.constData());
    if (reqHeader->op != 1) {
        return {};  // Only process BOOTREQUEST (op = 1)
    }

    // Verify Magic Cookie
    quint32 cookie = qFromBigEndian(reqHeader->magicCookie);
    if (cookie != kDhcpMagicCookie) {
        return {};
    }

    // Parse DHCP Options
    quint8 msgType = 0;
    bool isPxeClient = false;
    int offset = sizeof(DhcpHeader);

    while (offset < requestData.size()) {
        auto optionCode = static_cast<quint8>(requestData.at(offset++));
        if (optionCode == 255) {
            break;  // END option
        }
        if (optionCode == 0) {
            continue;  // PAD option
        }

        if (offset >= requestData.size()) {
            break;
        }
        auto optionLen = static_cast<quint8>(requestData.at(offset++));
        if (offset + optionLen > requestData.size()) {
            break;
        }

        QByteArray optionVal = requestData.mid(offset, optionLen);
        offset += optionLen;

        if (optionCode == 53 && optionLen >= 1) {
            msgType = static_cast<quint8>(optionVal.at(0));
        } else if (optionCode == 60) {
            QString vendorClass = QString::fromUtf8(optionVal);
            if (vendorClass.startsWith(QStringLiteral("PXEClient"), Qt::CaseInsensitive)) {
                isPxeClient = true;
            }
        }
    }

    // Only respond to PXE Clients (DHCPDISCOVER = 1 or DHCPREQUEST = 3)
    if (!isPxeClient || (msgType != 1 && msgType != 3)) {
        return {};
    }

    if (outClientMac) {
        *outClientMac = formatMacAddress(reqHeader->chaddr, reqHeader->hlen > 0 && reqHeader->hlen <= 16 ? reqHeader->hlen : 6);
    }

    // Craft ProxyDHCP Reply (BOOTREPLY = 2)
    QByteArray responseBuffer;
    responseBuffer.resize(sizeof(DhcpHeader));
    auto *respHeader = reinterpret_cast<DhcpHeader *>(responseBuffer.data());
    memset(respHeader, 0, sizeof(DhcpHeader));

    respHeader->op = 2;  // BOOTREPLY
    respHeader->htype = reqHeader->htype;
    respHeader->hlen = reqHeader->hlen;
    respHeader->hops = 0;
    respHeader->xid = reqHeader->xid;
    respHeader->secs = 0;
    respHeader->flags = reqHeader->flags;
    respHeader->ciaddr = 0;
    respHeader->yiaddr = 0;  // ProxyDHCP does not assign IP addresses

    // Set next server (siaddr) to TFTP server IP
    QHostAddress tftpAddr = tftpServerAddr.isNull() ? serverAddr : tftpServerAddr;
    respHeader->siaddr = qToBigEndian(tftpAddr.toIPv4Address());
    respHeader->giaddr = reqHeader->giaddr;

    memcpy(respHeader->chaddr, reqHeader->chaddr, 16);

    // Set sname and file in BOOTP header
    QByteArray snameUtf8 = tftpServerName.isEmpty() ? serverAddr.toString().toUtf8() : tftpServerName.toUtf8();
    qstrncpy(respHeader->sname, snameUtf8.constData(), sizeof(respHeader->sname));

    QByteArray fileUtf8 = bootFile.toUtf8();
    qstrncpy(respHeader->file, fileUtf8.constData(), sizeof(respHeader->file));

    respHeader->magicCookie = qToBigEndian(kDhcpMagicCookie);

    // Append DHCP Options
    const auto appendOption = [&responseBuffer](quint8 code, const QByteArray &data) {
        responseBuffer.append(static_cast<char>(code));
        responseBuffer.append(static_cast<char>(data.size()));
        responseBuffer.append(data);
    };

    // Option 53: DHCP Message Type (DHCPOFFER = 2 for DISCOVER, DHCPACK = 5 for REQUEST)
    quint8 replyMsgType = (msgType == 1) ? 2 : 5;
    responseBuffer.append(static_cast<char>(53));
    responseBuffer.append(static_cast<char>(1));
    responseBuffer.append(static_cast<char>(replyMsgType));

    // Option 54: Server Identifier
    quint32 serverIpBE = qToBigEndian(serverAddr.toIPv4Address());
    QByteArray serverIpData(reinterpret_cast<const char *>(&serverIpBE), 4);
    appendOption(54, serverIpData);

    // Option 60: Vendor Class Identifier ("PXEClient")
    appendOption(60, "PXEClient");

    // Option 66: TFTP Server Name / IP
    appendOption(66, snameUtf8);

    // Option 67: Bootfile Name
    appendOption(67, fileUtf8);

    // Option 255: END
    responseBuffer.append(static_cast<char>(255));

    return responseBuffer;
}

}  // namespace tftp
