#pragma once

#include "core/tftp_protocol.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QElapsedTimer>
#include <QPointer>

#include <memory>

class QUdpSocket;
class QTimer;
class QFile;

namespace tftp {

/**
 * @brief One active server-side TFTP transfer (RRQ or WRQ).
 *
 * The server creates a TftpSession per accepted request. Each session binds
 * its own ephemeral UDP socket (its Transfer ID, RFC 1350 §4) and runs the
 * DATA/ACK loop with retransmission and timeout to completion, then emits
 * @ref finished() exactly once.
 *
 * @note Lives in @c src/core/ and links only against Qt6::Core / Qt6::Network.
 */
class TftpSession : public QObject {
    Q_OBJECT
public:
    /**
     * @brief Construct a session for a single accepted request.
     * @param peer Client address (the request's source).
     * @param peerPort Client source port (the client's TID).
     * @param request The parsed RRQ/WRQ that initiated this transfer.
     * @param filePath Sandbox-resolved absolute path to read or write.
     * @param parent Optional QObject parent.
     */
    TftpSession(const QHostAddress &peer, quint16 peerPort, const Request &request, QString filePath, QObject *parent = nullptr);
    ~TftpSession() override;

    /**
     * @brief Bind the ephemeral socket and kick off the transfer.
     * @return @c false if the socket cannot be bound. A protocol-level failure
     *         still returns @c true here and is reported via @ref finished().
     */
    bool start();

    /** @brief Set single-port mode for this session. */
    void setSinglePortMode(bool enabled) { m_singlePortMode = enabled; }
    /** @return @c true if single-port mode is enabled. */
    bool isSinglePortMode() const { return m_singlePortMode; }
    /** @brief Process an incoming datagram routed from single-port server. */
    void processDatagram(const QByteArray &buf);
    /** @brief Expose internal socket for routing or address check. */
    QUdpSocket *socket() const { return m_socket.get(); }

    /** @return The file name being transferred. */
    QString filename() const { return m_request.filename; }
    /** @return Bytes transferred so far. */
    qint64 bytesTransferred() const { return m_bytesTransferred; }
    /** @return The negotiated block size in bytes (RFC 2348). */
    int negotiatedBlockSize() const { return m_blockSize; }
    /** @return The current block number. */
    quint16 blockCount() const { return m_currentBlock; }

    /** @brief Set the session-level speed limit in bytes per second (0 means
     * unlimited). */
    void setSessionLimit(qint64 bytesPerSec) { m_sessionLimit = bytesPerSec; }
    /** @return The session-level speed limit in bytes per second. */
    qint64 sessionLimit() const { return m_sessionLimit; }

    /** @return The client IP address. */
    QHostAddress peerAddress() const { return m_peer; }
    /** @return The client port. */
    quint16 peerPort() const { return m_peerPort; }

signals:
    /**
     * @brief Emitted exactly once when the transfer ends.
     * @param ok @c true on success.
     * @param message Failure cause when @p ok is @c false.
     */
    void finished(bool ok, const QString &message);

    /**
     * @brief Emitted after each processed block.
     * @param bytesTransferred Cumulative bytes transferred.
     * @param totalBytes Total size if known, otherwise -1.
     */
    void progress(qint64 bytesTransferred, qint64 totalBytes);

    /** @brief Emitted when a packet is retransmitted. */
    void retransmissionOccurred();

private slots:
    /** @brief Read and dispatch pending datagrams on the session socket. */
    void onReadyRead();
    /** @brief Retransmit the last packet or fail after exhausting retries. */
    void onTimeout();
    /** @brief Send the pending deferred packet. */
    void onSendTimerTimeout();
    void onDataBlockRead(qint64 block, const QByteArray &payload, bool ok);
    void onDataBlockWritten(qint64 block, int size, bool ok);

private:
    void sendPacketImmediate(const QByteArray &packet);
    void sendPacketDeferred(const QByteArray &packet, bool armRetrans);
    void sendError(ErrorCode code, const QString &message);
    void finish(bool ok, const QString &message);

    // RRQ (we send the file).
    void sendDataBlock(qint64 block);  ///< (Re)send DATA for @p block.
    void handleAck(quint16 block);

    // WRQ (we receive the file).
    void sendAck(qint64 block);  ///< (Re)send an ACK for @p block.
    void handleData(quint16 block, const QByteArray &payload);

    /// Build the accepted option set and (when non-empty) arm the OACK phase.
    Options negotiateOptions(qint64 fileSize);
    void armRetransmit();
    qint64 requestSessionDelay(qint64 packetSize);

    QHostAddress m_peer;
    quint16 m_peerPort = 0;
    Request m_request;
    QString m_filePath;

    std::unique_ptr<QUdpSocket> m_socket;
    std::unique_ptr<QFile> m_file;
    QTimer *m_timer = nullptr;
    QTimer *m_sendTimer = nullptr;

    bool m_isRead = false;  ///< true: RRQ (sending); false: WRQ (receiving).
    int m_blockSize = kDefaultBlockSize;
    int m_timeoutMs = 5000;  ///< Per-attempt timeout (RFC 2349 timeout option).
    int m_windowSize = 1;    ///< Negotiated window size (RFC 7440).
    int m_retries = 0;
    int m_maxRetries = 5;

    qint64 m_currentBlock = 0;     ///< Last block we are expecting/serving.
    QByteArray m_lastPacket;       ///< Last datagram sent, for retransmission.
    bool m_oackPending = false;    ///< Awaiting ACK-0 / first DATA after OACK.
    bool m_sentLastBlock = false;  ///< RRQ: final (short) block has been sent.
    bool m_finished = false;
    bool m_emitted = false;  ///< Ensures finished() fires exactly once.
    bool m_singlePortMode = false;

    qint64 m_bytesTransferred = 0;
    qint64 m_totalBytes = -1;  ///< Known for RRQ; -1 for WRQ unless tsize given.

    qint64 m_sessionLimit = 0;
    double m_sessionTokens = 0;
    QElapsedTimer m_sessionTokenTimer;
    QByteArray m_pendingSendPacket;
    bool m_pendingArmRetransmit = false;

    struct ReadTask;
    struct WriteTask;
};

}  // namespace tftp
