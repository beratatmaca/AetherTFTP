#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QUdpSocket>

namespace tftp {

/**
 * @brief Lightweight ProxyDHCP server for PXE network booting.
 *
 * Listens on UDP port 67 (or configured port) for DHCPDISCOVER/DHCPREQUEST packets
 * carrying DHCP Option 60 ("PXEClient"). Responds with DHCPOFFER/DHCPACK packets
 * specifying the TFTP Boot Server address (Option 66 / siaddr) and Boot File Name (Option 67 / file).
 *
 * Operates alongside existing network DHCP servers without interfering with IP address allocation.
 */
class PxeProxyDhcp : public QObject {
    Q_OBJECT
public:
    explicit PxeProxyDhcp(QObject *parent = nullptr);
    ~PxeProxyDhcp() override;

    /**
     * @brief Start listening for ProxyDHCP requests.
     * @param port UDP port (default 67).
     * @param bindAddr Interface address to bind (default AnyIPv4).
     * @return @c true on success.
     */
    bool listen(quint16 port = 67, const QHostAddress &bindAddr = QHostAddress::AnyIPv4);

    /** @brief Stop listening. */
    void close();

    /** @return @c true if socket is listening. */
    bool isListening() const;

    /** @return The listening port. */
    quint16 port() const;

    /** @brief Set the boot file name (e.g. "bootx64.efi" or "pxelinux.0"). */
    void setBootFile(const QString &bootFile) { m_bootFile = bootFile; }
    /** @return The boot file name. */
    QString bootFile() const { return m_bootFile; }

    /** @brief Set the TFTP server IP address (Option 66 / siaddr). */
    void setTftpServerAddress(const QHostAddress &addr) { m_tftpServerAddr = addr; }
    /** @return The TFTP server IP address. */
    QHostAddress tftpServerAddress() const { return m_tftpServerAddr; }

    /** @brief Set the TFTP server hostname. */
    void setTftpServerName(const QString &name) { m_tftpServerName = name; }
    /** @return The TFTP server hostname. */
    QString tftpServerName() const { return m_tftpServerName; }

    /**
     * @brief Parse a raw DHCP datagram and craft a ProxyDHCP response if applicable.
     * Exposed for unit testing without socket I/O.
     */
    static QByteArray processDhcpPacket(const QByteArray &requestData, const QHostAddress &serverAddr, const QString &bootFile,
                                        const QHostAddress &tftpServerAddr, const QString &tftpServerName, QString *outClientMac = nullptr);

signals:
    /** @brief Emitted when a PXE boot request is served. */
    void requestHandled(const QString &clientMac, const QHostAddress &clientIp, const QString &bootFile);

    /** @brief Emitted for diagnostic log messages. */
    void logMessage(const QString &message);

private slots:
    void onReadyRead();

private:
    QUdpSocket *m_socket = nullptr;
    QString m_bootFile = QStringLiteral("bootx64.efi");
    QHostAddress m_tftpServerAddr = QHostAddress::LocalHost;
    QString m_tftpServerName;
};

}  // namespace tftp
