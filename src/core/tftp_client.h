#pragma once

#include "core/tftp_protocol.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <memory>

class QUdpSocket;
class QTimer;
class QFile;

namespace tftp {

/**
 * @brief Drives a single client-side upload (put) or download (get) transfer.
 *
 * Asynchronous and signal-based: call @ref downloadFile() / @ref uploadFile(),
 * then wait for @ref transferFinished(). One instance handles one transfer at
 * a time. Negotiates RFC 2348 @c blksize and RFC 2349 @c tsize / @c timeout
 * options, and recovers from loss via retransmission.
 *
 * @note Lives in @c src/core/ and links only against Qt6::Core / Qt6::Network.
 */
class TftpClient : public QObject {
    Q_OBJECT
public:
    /**
     * @brief Construct an idle client.
     * @param parent Optional QObject parent.
     */
    explicit TftpClient(QObject *parent = nullptr);
    ~TftpClient() override;

    /**
     * @brief Set the block size to request for the next transfer (RFC 2348).
     * @param blockSize Desired block size in bytes; clamped to [8, 65464].
     * @note The value actually agreed is reported by @ref
     * negotiatedBlockSize().
     */
    void setBlockSize(int blockSize) { m_requestedBlockSize = blockSize; }
    /** @return The block size agreed with the server (RFC 2348). */
    int negotiatedBlockSize() const { return m_blockSize; }

    /**
     * @brief Set the window size to request for the next transfer (RFC 7440).
     * @param windowSize Desired window size in packets; clamped to [1, 64].
     */
    void setWindowSize(int windowSize) { m_requestedWindowSize = windowSize; }
    /** @return The window size agreed with the server (RFC 7440). */
    int negotiatedWindowSize() const { return m_windowSize; }

    /**
     * @brief Set the per-attempt retransmission timeout.
     * @param milliseconds Timeout in milliseconds; also offered to the server
     *        as the RFC 2349 @c timeout option (rounded to whole seconds).
     */
    void setTimeout(int milliseconds) { m_timeoutMs = milliseconds; }

    /**
     * @brief Download @p remoteFile from @p host into @p localPath.
     * @param host Server host name or address.
     * @param port Server port.
     * @param remoteFile Remote file name to request (RRQ).
     * @param localPath Local path to write the downloaded data to.
     */
    void downloadFile(const QString &host, quint16 port, const QString &remoteFile, const QString &localPath);

    /**
     * @brief Upload @p localPath to @p host as @p remoteFile.
     * @param host Server host name or address.
     * @param port Server port.
     * @param localPath Local file to read and send.
     * @param remoteFile Remote file name to create (WRQ).
     */
    void uploadFile(const QString &host, quint16 port, const QString &localPath, const QString &remoteFile);

    /** @return @c true while a transfer is in progress. */
    bool isRunning() const { return m_running; }

    /**
     * @brief Cancel the in-progress transfer, if any.
     *
     * Notifies the peer with an ERROR datagram (RFC 1350), tears down the
     * socket/timer/file, discards a partially written download, and emits
     * @ref transferFinished(false). A no-op when no transfer is running.
     */
    void abort();

signals:
    /**
     * @brief Emitted exactly once when the transfer ends.
     * @param ok @c true on success.
     */
    void transferFinished(bool ok);

    /**
     * @brief Emitted on any network or file error.
     * @param message Human-readable error description.
     */
    void errorOccurred(const QString &message);

    /**
     * @brief Emitted after each processed block.
     * @param bytesTransferred Cumulative bytes transferred.
     * @param totalBytes Total size if known, otherwise -1.
     */
    void progress(qint64 bytesTransferred, qint64 totalBytes);

private slots:
    /** @brief Read and dispatch pending datagrams on the client socket. */
    void onReadyRead();
    /** @brief Retransmit the last packet or fail after exhausting retries. */
    void onTimeout();

private:
    /** @brief Direction of the active transfer. */
    enum class Mode : quint8 { Idle, Download, Upload };

    bool begin(Mode mode, const QString &host, quint16 port, const QString &remoteFile, const QString &localPath);
    void sendInitialRequest();
    void applyOack(const Options &options);

    // Download path.
    void handleData(quint16 block, const QByteArray &payload);
    // Upload path.
    void handleAck(quint16 block);
    void sendDataBlock(qint64 block);

    void armRetransmit();
    void fail(const QString &message);
    void succeed();

    std::unique_ptr<QUdpSocket> m_socket;
    std::unique_ptr<QFile> m_file;
    QTimer *m_timer = nullptr;

    Mode m_mode = Mode::Idle;
    bool m_running = false;
    QHostAddress m_serverAddr;
    quint16 m_serverPort = 0;  ///< Initial request port; updated to the TID.
    bool m_tidLocked = false;  ///< Server's transfer TID has been learned.
    QString m_remoteFile;
    QString m_localPath;

    int m_requestedBlockSize = kDefaultBlockSize;
    int m_blockSize = kDefaultBlockSize;
    int m_requestedWindowSize = 1;
    int m_windowSize = 1;
    int m_timeoutMs = 5000;
    int m_retries = 0;
    int m_maxRetries = 5;

    qint64 m_block = 0;       ///< Download: last received; upload: last sent.
    QByteArray m_lastPacket;  ///< Last datagram sent, for retransmission.
    bool m_optionsRequested = false;
    bool m_awaitingFirstReply = false;  ///< Expecting OACK or first DATA/ACK.
    bool m_lastBlockSent = false;       ///< Upload: final short block dispatched.

    qint64 m_bytesTransferred = 0;
    qint64 m_totalBytes = -1;
};

}  // namespace tftp
