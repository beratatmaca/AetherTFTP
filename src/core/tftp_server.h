#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <QList>
#include <QMap>

class QUdpSocket;

namespace tftp {

class TftpSession;
class MetricsExporter;

/**
 * @brief TFTP listener that dispatches each request to its own session.
 *
 * Binds the well-known (or configured) UDP port and creates a @ref TftpSession
 * for each incoming RRQ/WRQ. File access is sandboxed to the configured root
 * directory; any path that escapes the root is rejected with an
 * @c AccessViolation error.
 *
 * @note Lives in @c src/core/ and links only against Qt6::Core / Qt6::Network.
 */
class TftpServer : public QObject {
    Q_OBJECT
public:
    /**
     * @brief Construct an idle server.
     * @param parent Optional QObject parent.
     */
    explicit TftpServer(QObject *parent = nullptr);
    ~TftpServer() override;

    /**
     * @brief Bind the listening socket and begin serving @p rootDir.
     * @param address Local address to bind.
     * @param port UDP port to bind (0 lets the OS pick a free port).
     * @param rootDir Directory whose files are served / written.
     * @return @c true on success; on failure see @ref lastError().
     */
    bool listen(const QHostAddress &address, quint16 port,
                const QString &rootDir);

    /** @brief Close the listening socket and stop accepting requests. */
    void close();

    /** @return @c true while the listening socket is bound. */
    bool isListening() const;
    /** @return The bound local port, or 0 if not listening. */
    quint16 port() const;
    /** @return The absolute served root directory. */
    QString rootDir() const { return m_rootDir; }
    /** @return The last error string set by @ref listen(). */
    QString lastError() const { return m_lastError; }
    /** @return The number of in-flight transfers. */
    int activeSessions() const { return m_activeSessions; }
    /** @return Cumulative total bytes transferred. */
    qint64 totalBytesTransferred() const;
    /** @return Total successful transfers. */
    qint64 transfersSuccess() const { return m_transfersSuccess; }
    /** @return Total failed transfers. */
    qint64 transfersFailure() const { return m_transfersFailure; }
    /** @return Total packet retransmissions. */
    qint64 retransmissionCount() const { return m_retransmissionCount; }

    /**
     * @brief Resolve a client file name to a safe absolute path in the sandbox.
     * @param filename Client-supplied file name.
     * @return Absolute path inside the root, or empty if it escapes the
     * sandbox.
     */
    QString resolveSafePath(const QString &filename) const;

    /** @brief Set the server to read-only mode. */
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    /** @return @c true if the server is in read-only mode. */
    bool isReadOnly() const { return m_readOnly; }

    /** @brief Set the list of CIDR subnets allowed to access the server. Empty
     * means allow all. */
    void setWhitelist(const QList<QString> &cidrs);
    /** @brief Set the list of CIDR subnets blocked from accessing the server.
     */
    void setBlacklist(const QList<QString> &cidrs);
    /** @brief Check if the given client address and transfer type are allowed
     * by ACL rules. */
    bool isAllowed(const QHostAddress &clientAddress, bool isUpload) const;

    /** @brief Enable/disable single-port multiplexing. */
    void setSinglePortMode(bool enabled) { m_singlePortMode = enabled; }
    /** @return @c true if single-port multiplexing is enabled. */
    bool isSinglePortMode() const { return m_singlePortMode; }

    /** @brief Write a UDP datagram from the server socket (used by sessions in
     * single-port mode). */
    void sendSessionPacket(const QByteArray &packet,
                           const QHostAddress &address, quint16 port);

    /** @brief Set the global bandwidth limit in bytes per second (0 means
     * unlimited). */
    void setGlobalLimit(qint64 bytesPerSec) { m_globalLimit = bytesPerSec; }
    /** @return The global bandwidth limit in bytes per second. */
    qint64 globalLimit() const { return m_globalLimit; }

    /** @brief Set the per-session speed limit in bytes per second (0 means
     * unlimited). */
    void setSessionLimit(qint64 bytesPerSec) { m_sessionLimit = bytesPerSec; }
    /** @return The per-session speed limit in bytes per second. */
    qint64 sessionLimit() const { return m_sessionLimit; }

    /** @brief Calculate the delay for a packet of size packetSize based on
     * global limit. */
    qint64 requestGlobalDelay(qint64 packetSize);

    /** @brief Enable/disable JSON-formatted logging. */
    void setJsonLoggingEnabled(bool enabled) { m_jsonLoggingEnabled = enabled; }
    /** @return @c true if JSON logging is enabled. */
    bool isJsonLoggingEnabled() const { return m_jsonLoggingEnabled; }

    /** @brief Write log events in structured JSON format or plain text. */
    void logEvent(const QString &eventType, const QString &sessionId,
                  const QString &clientIp, const QString &fileName,
                  int blockCount, const QString &status,
                  const QString &message = QString());

    /** @brief Format server metrics for Prometheus exporter. */
    QString getMetricsFormatted() const;

    /** @brief Start the metrics exporter server on @p port. */
    bool startMetricsServer(quint16 port);
    /** @brief Stop the metrics exporter server. */
    void stopMetricsServer();
    /** @return The metrics server port, or 0 if not running. */
    quint16 metricsServerPort() const;

signals:
    /**
     * @brief Emitted when a new transfer is accepted.
     * @param filename Requested file name.
     * @param isUpload @c true for WRQ (upload), @c false for RRQ (download).
     */
    void transferStarted(const QString &filename, bool isUpload);

    /**
     * @brief Emitted when a transfer ends.
     * @param filename Requested file name.
     * @param ok @c true on success.
     * @param message Failure cause when @p ok is @c false.
     */
    void transferFinished(const QString &filename, bool ok,
                          const QString &message);

    /**
     * @brief Emitted for human-readable diagnostic log lines.
     * @param message The log message.
     */
    void logMessage(const QString &message);

private slots:
    /** @brief Read pending requests on the listening socket and dispatch them.
     */
    void onReadyRead();

private:
    struct SubnetRule {
        QHostAddress address;
        int prefixLength;
        bool matches(const QHostAddress &ip) const;
    };

    QUdpSocket *m_socket = nullptr;
    QString m_rootDir;
    QString m_lastError;
    int m_activeSessions = 0;
    bool m_readOnly = false;
    bool m_singlePortMode = false;
    QList<SubnetRule> m_whitelist;
    QList<SubnetRule> m_blacklist;
    QMap<QString, TftpSession *> m_sessions;

    // Rate Limiting
    qint64 m_globalLimit = 0;
    qint64 m_sessionLimit = 0;
    mutable double m_globalTokens = 0;
    mutable QElapsedTimer m_globalTimer;

    // JSON Logging
    bool m_jsonLoggingEnabled = false;

    // Prometheus Exporter
    MetricsExporter *m_metricsServer = nullptr;
    mutable qint64 m_historicalBytesTransferred = 0;
    mutable qint64 m_transfersSuccess = 0;
    mutable qint64 m_transfersFailure = 0;
    mutable qint64 m_retransmissionCount = 0;
};

}  // namespace tftp
