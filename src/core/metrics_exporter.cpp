#include "core/metrics_exporter.h"

#include "core/tftp_server.h"

#include <QTcpSocket>
#include <QTimer>

namespace tftp {

namespace {
// Defensive limits for the network-facing endpoint.
constexpr int kMaxRequestBytes = 16 * 1024;  ///< Cap request header size.
constexpr int kRequestTimeoutMs = 5000;      ///< Per-connection deadline.
constexpr int kMaxConnections = 64;          ///< Concurrent connection cap.
}  // namespace

MetricsExporter::MetricsExporter(TftpServer *server, QObject *parent)
    : QTcpServer(parent), m_server(server) {}

void MetricsExporter::incomingConnection(qintptr socketDescriptor) {
    auto *socket = new QTcpSocket(this);
    if (!socket->setSocketDescriptor(socketDescriptor)) {
        socket->deleteLater();
        return;
    }

    // Always reclaim per-connection state when the socket goes away.
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        m_connections.remove(socket);
        socket->deleteLater();
    });

    // Shed load: refuse to track more than kMaxConnections at once.
    if (m_connections.size() >= kMaxConnections) {
        sendResponse(socket, 503, "Service Unavailable", "text/plain",
                     "too many connections\n");
        return;
    }

    m_connections.insert(socket, Connection{});

    // Slowloris guard: bound the lifetime of a slow or idle request.
    auto *deadline = new QTimer(socket);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [this, socket]() {
        auto it = m_connections.find(socket);
        if (it == m_connections.end() || it->responded)
            return;  // Already answered or gone.
        sendResponse(socket, 408, "Request Timeout", "text/plain",
                     "request timeout\n");
    });
    deadline->start(kRequestTimeoutMs);

    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        auto it = m_connections.find(socket);
        if (it == m_connections.end() || it->responded) {
            socket->readAll();  // Drain and ignore anything post-response.
            return;
        }
        it->buffer.append(socket->readAll());

        if (it->buffer.size() > kMaxRequestBytes) {
            sendResponse(socket, 431, "Request Header Fields Too Large",
                         "text/plain", "request too large\n");
            return;
        }

        // Wait until the full request header block has arrived.
        const qsizetype headerEnd = it->buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        handleRequest(socket, it->buffer.left(headerEnd));
    });
}

void MetricsExporter::handleRequest(QTcpSocket *socket,
                                    const QByteArray &headerBlock) {
    // Request line: METHOD SP PATH SP VERSION
    const qsizetype lineEnd = headerBlock.indexOf("\r\n");
    const QByteArray requestLine =
        lineEnd < 0 ? headerBlock : headerBlock.left(lineEnd);
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() < 2) {
        sendResponse(socket, 400, "Bad Request", "text/plain", "bad request\n");
        return;
    }

    const QByteArray &method = parts.at(0);
    QByteArray path = parts.at(1);
    const qsizetype query = path.indexOf('?');
    if (query >= 0)
        path = path.left(query);

    if (method != "GET") {
        sendResponse(socket, 405, "Method Not Allowed", "text/plain",
                     "method not allowed\n");
        return;
    }
    if (path != "/metrics" && path != "/") {
        sendResponse(socket, 404, "Not Found", "text/plain", "not found\n");
        return;
    }
    if (!m_server) {
        sendResponse(socket, 503, "Service Unavailable", "text/plain",
                     "metrics source unavailable\n");
        return;
    }

    sendResponse(socket, 200, "OK", "text/plain; version=0.0.4; charset=utf-8",
                 m_server->getMetricsFormatted().toUtf8());
}

void MetricsExporter::sendResponse(QTcpSocket *socket, int statusCode,
                                   const QByteArray &reason,
                                   const QByteArray &contentType,
                                   const QByteArray &body) {
    if (auto it = m_connections.find(socket); it != m_connections.end()) {
        if (it->responded)
            return;  // Never send a second response on one connection.
        it->responded = true;
    }

    QByteArray response;
    response.append("HTTP/1.1 ");
    response.append(QByteArray::number(statusCode));
    response.append(' ');
    response.append(reason);
    response.append("\r\n");
    response.append("Content-Type: ").append(contentType).append("\r\n");
    response.append("Content-Length: ")
        .append(QByteArray::number(body.size()))
        .append("\r\n");
    response.append("Connection: close\r\n\r\n");
    response.append(body);

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

}  // namespace tftp
