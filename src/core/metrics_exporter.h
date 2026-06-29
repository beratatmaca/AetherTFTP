#pragma once

#include <QByteArray>
#include <QHash>
#include <QTcpServer>

class QTcpSocket;

namespace tftp {

class TftpServer;

/**
 * @brief A HTTP server exposing Prometheus metrics.
 *
 * Serves @c GET @c /metrics in the Prometheus text exposition format. The
 * implementation is deliberately defensive against the failure modes of a
 * hand-rolled HTTP endpoint exposed on the network:
 *   - a per-connection deadline bounds slow/idle clients (slowloris);
 *   - the request is read until the end of headers, capped in size;
 *   - the request line is validated (only @c GET @c /metrics is served);
 *   - exactly one response is sent per connection;
 *   - the number of concurrent connections is capped to shed floods.
 */
class MetricsExporter : public QTcpServer {
    Q_OBJECT
public:
    /**
     * @brief Construct the exporter bound to a metrics source.
     * @param server The TftpServer whose metrics are exposed (not owned).
     * @param parent Optional QObject parent.
     */
    explicit MetricsExporter(TftpServer *server, QObject *parent = nullptr);
    ~MetricsExporter() override = default;

protected:
    /** @brief Accept a connection and drive the bounded request/response. */
    void incomingConnection(qintptr socketDescriptor) override;

private:
    /** @brief Per-connection parsing state. */
    struct Connection {
        QByteArray buffer;       ///< Accumulated request bytes (headers).
        bool responded = false;  ///< Guards against multiple responses.
    };

    /** @brief Parse the buffered request and dispatch a single response. */
    void handleRequest(QTcpSocket *socket, const QByteArray &headerBlock);

    /** @brief Write one HTTP response and begin closing the connection. */
    void sendResponse(QTcpSocket *socket, int statusCode, const QByteArray &reason, const QByteArray &contentType, const QByteArray &body);

    TftpServer *m_server = nullptr;
    QHash<QTcpSocket *, Connection> m_connections;
};

}  // namespace tftp
