#include "core/embedded_web_server.h"
#include "core/tftp_server.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace tftp {

EmbeddedWebServer::EmbeddedWebServer(TftpServer *tftpServer, QObject *parent)
    : QObject(parent), m_tftpServer(tftpServer), m_tcpServer(new QTcpServer(this)) {
    connect(m_tcpServer, &QTcpServer::newConnection, this, &EmbeddedWebServer::onNewConnection);
}

EmbeddedWebServer::~EmbeddedWebServer() {
    close();
}

bool EmbeddedWebServer::listen(const QHostAddress &address, quint16 port) {
    close();
    return m_tcpServer->listen(address, port);
}

void EmbeddedWebServer::close() {
    if (m_tcpServer && m_tcpServer->isListening()) {
        m_tcpServer->close();
    }
}

bool EmbeddedWebServer::isListening() const {
    return m_tcpServer && m_tcpServer->isListening();
}

quint16 EmbeddedWebServer::port() const {
    return m_tcpServer ? m_tcpServer->serverPort() : 0;
}

void EmbeddedWebServer::onNewConnection() {
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &EmbeddedWebServer::onClientReadyRead);
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void EmbeddedWebServer::onClientReadyRead() {
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    QByteArray data = socket->readAll();
    QString reqStr = QString::fromUtf8(data);
    QStringList lines = reqStr.split(QStringLiteral("\r\n"));
    if (lines.isEmpty())
        return;

    QStringList requestLineTokens = lines.first().split(QLatin1Char(' '));
    if (requestLineTokens.size() < 2)
        return;

    QString method = requestLineTokens.at(0).toUpper();
    QString path = requestLineTokens.at(1);

    // Extract body if present
    int headerEnd = data.indexOf("\r\n\r\n");
    QByteArray body = (headerEnd >= 0) ? data.mid(headerEnd + 4) : QByteArray();

    handleRequest(socket, method, path, body);
}

void EmbeddedWebServer::handleRequest(QTcpSocket *socket, const QString &method, const QString &path, const QByteArray &body) {
    Q_UNUSED(body);

    if (method == QStringLiteral("GET")) {
        if (path == QStringLiteral("/") || path == QStringLiteral("/index.html")) {
            sendResponse(socket, 200, QStringLiteral("OK"), QStringLiteral("text/html; charset=utf-8"), getEmbeddedHtmlPage());
            return;
        }

        if (path == QStringLiteral("/api/status")) {
            QJsonObject root;
            if (m_tftpServer) {
                root.insert(QStringLiteral("isListening"), m_tftpServer->isListening());
                root.insert(QStringLiteral("port"), m_tftpServer->port());
                root.insert(QStringLiteral("rootDir"), m_tftpServer->rootDir());
                root.insert(QStringLiteral("activeSessions"), m_tftpServer->activeSessions());
                root.insert(QStringLiteral("totalBytesTransferred"), m_tftpServer->totalBytesTransferred());
                root.insert(QStringLiteral("transfersSuccess"), m_tftpServer->transfersSuccess());
                root.insert(QStringLiteral("transfersFailure"), m_tftpServer->transfersFailure());
                root.insert(QStringLiteral("retransmissions"), m_tftpServer->retransmissionCount());
                root.insert(QStringLiteral("proxyDhcpEnabled"), m_tftpServer->isProxyDhcpEnabled());
                root.insert(QStringLiteral("proxyBootFile"), m_tftpServer->proxyDhcpBootFile());
            } else {
                root.insert(QStringLiteral("isListening"), false);
            }
            sendResponse(socket, 200, QStringLiteral("OK"), QStringLiteral("application/json"), QJsonDocument(root).toJson());
            return;
        }

        if (path == QStringLiteral("/metrics")) {
            QString metricsStr;
            if (m_tftpServer) {
                metricsStr += QStringLiteral("# HELP aethertftp_active_sessions Active TFTP sessions\n");
                metricsStr += QStringLiteral("# TYPE aethertftp_active_sessions gauge\n");
                metricsStr += QStringLiteral("aethertftp_active_sessions %1\n").arg(m_tftpServer->activeSessions());

                metricsStr += QStringLiteral("# HELP aethertftp_bytes_transferred_total Total bytes transferred\n");
                metricsStr += QStringLiteral("# TYPE aethertftp_bytes_transferred_total counter\n");
                metricsStr += QStringLiteral("aethertftp_bytes_transferred_total %1\n").arg(m_tftpServer->totalBytesTransferred());

                metricsStr += QStringLiteral("# HELP aethertftp_transfers_success_total Total successful transfers\n");
                metricsStr += QStringLiteral("# TYPE aethertftp_transfers_success_total counter\n");
                metricsStr += QStringLiteral("aethertftp_transfers_success_total %1\n").arg(m_tftpServer->transfersSuccess());

                metricsStr += QStringLiteral("# HELP aethertftp_transfers_failure_total Total failed transfers\n");
                metricsStr += QStringLiteral("# TYPE aethertftp_transfers_failure_total counter\n");
                metricsStr += QStringLiteral("aethertftp_transfers_failure_total %1\n").arg(m_tftpServer->transfersFailure());
            }
            sendResponse(socket, 200, QStringLiteral("OK"), QStringLiteral("text/plain; version=0.0.4"), metricsStr.toUtf8());
            return;
        }
    }

    sendResponse(socket, 404, QStringLiteral("Not Found"), QStringLiteral("text/plain"), "404 Not Found");
}

void EmbeddedWebServer::sendResponse(QTcpSocket *socket, int statusCode, const QString &statusText, const QString &contentType,
                                     const QByteArray &body) {
    QByteArray header;
    header.append(QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    header.append(QStringLiteral("Content-Type: %1\r\n").arg(contentType).toUtf8());
    header.append(QStringLiteral("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    header.append("Connection: close\r\n");
    header.append("Access-Control-Allow-Origin: *\r\n");
    header.append("\r\n");

    socket->write(header);
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

QByteArray EmbeddedWebServer::getEmbeddedHtmlPage() {
    static const char html[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>AetherTFTP — Web Dashboard</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;700&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-primary: #0a0c10;
            --bg-card: rgba(22, 27, 34, 0.75);
            --border-color: rgba(255, 255, 255, 0.1);
            --accent-cyan: #00f2fe;
            --accent-blue: #4facfe;
            --accent-purple: #7f00ff;
            --accent-green: #00e676;
            --text-main: #f0f6fc;
            --text-muted: #8b949e;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Outfit', sans-serif;
            background-color: var(--bg-primary);
            color: var(--text-main);
            min-height: 100vh;
            padding: 2rem;
            background-image: 
                radial-gradient(at 0% 0%, rgba(127, 0, 255, 0.15) 0px, transparent 50%),
                radial-gradient(at 100% 100%, rgba(0, 242, 254, 0.15) 0px, transparent 50%);
        }
        .container { max-width: 1200px; margin: 0 auto; }
        header {
            display: flex; justify-content: space-between; align-items: center;
            margin-bottom: 2rem; padding-bottom: 1rem; border-bottom: 1px solid var(--border-color);
        }
        .logo { display: flex; align-items: center; gap: 0.75rem; font-size: 1.75rem; font-weight: 700; background: linear-gradient(135deg, var(--accent-cyan), var(--accent-blue)); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
        .status-badge {
            display: inline-flex; align-items: center; gap: 0.5rem;
            padding: 0.5rem 1rem; border-radius: 9999px; font-weight: 600; font-size: 0.875rem;
            background: rgba(0, 230, 118, 0.1); color: var(--accent-green); border: 1px solid rgba(0, 230, 118, 0.2);
        }
        .status-pulse { width: 8px; height: 8px; border-radius: 50%; background-color: var(--accent-green); box-shadow: 0 0 10px var(--accent-green); animation: pulse 2s infinite; }
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.4; } 100% { opacity: 1; } }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 1.5rem; margin-bottom: 2rem; }
        .card {
            background: var(--bg-card); backdrop-filter: blur(12px); border: 1px solid var(--border-color);
            border-radius: 1rem; padding: 1.5rem; transition: transform 0.2s, border-color 0.2s;
        }
        .card:hover { transform: translateY(-2px); border-color: rgba(255, 255, 255, 0.2); }
        .card-label { font-size: 0.875rem; color: var(--text-muted); font-weight: 500; margin-bottom: 0.5rem; text-transform: uppercase; letter-spacing: 0.05em; }
        .card-value { font-size: 2rem; font-weight: 700; font-family: 'JetBrains Mono', monospace; }
        .section-title { font-size: 1.25rem; font-weight: 600; margin-bottom: 1rem; }
        .info-panel { background: var(--bg-card); backdrop-filter: blur(12px); border: 1px solid var(--border-color); border-radius: 1rem; padding: 1.5rem; }
        .info-row { display: flex; justify-content: space-between; padding: 0.75rem 0; border-bottom: 1px solid var(--border-color); }
        .info-row:last-child { border-bottom: none; }
        .mono { font-family: 'JetBrains Mono', monospace; }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div class="logo">⚡ AetherTFTP Dashboard</div>
            <div class="status-badge" id="statusBadge">
                <div class="status-pulse"></div>
                <span id="statusText">ONLINE</span>
            </div>
        </header>

        <div class="grid">
            <div class="card">
                <div class="card-label">Active Sessions</div>
                <div class="card-value" id="valActive">0</div>
            </div>
            <div class="card">
                <div class="card-label">Bytes Transferred</div>
                <div class="card-value" id="valBytes">0 B</div>
            </div>
            <div class="card">
                <div class="card-label">Success Rate</div>
                <div class="card-value" id="valSuccess">0 / 0</div>
            </div>
            <div class="card">
                <div class="card-label">Retransmissions</div>
                <div class="card-value" id="valRetrans">0</div>
            </div>
        </div>

        <div class="info-panel">
            <div class="section-title">Server Runtime Details</div>
            <div class="info-row">
                <span class="text-muted">TFTP Port:</span>
                <span class="mono" id="valPort">69</span>
            </div>
            <div class="info-row">
                <span class="text-muted">Root Directory:</span>
                <span class="mono" id="valDir">/var/tftp</span>
            </div>
            <div class="info-row">
                <span class="text-muted">PXE ProxyDHCP:</span>
                <span class="mono" id="valPxe">Disabled</span>
            </div>
        </div>
    </div>

    <script>
        function formatBytes(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
        }

        async function updateStats() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                if (data.isListening) {
                    document.getElementById('statusText').innerText = 'ONLINE (Port ' + data.port + ')';
                    document.getElementById('valActive').innerText = data.activeSessions;
                    document.getElementById('valBytes').innerText = formatBytes(data.totalBytesTransferred);
                    document.getElementById('valSuccess').innerText = data.transfersSuccess + ' ok / ' + data.transfersFailure + ' err';
                    document.getElementById('valRetrans').innerText = data.retransmissions;
                    document.getElementById('valPort').innerText = data.port;
                    document.getElementById('valDir').innerText = data.rootDir || 'N/A';
                    document.getElementById('valPxe').innerText = data.proxyDhcpEnabled ? ('Enabled (' + data.proxyBootFile + ')') : 'Disabled';
                } else {
                    document.getElementById('statusText').innerText = 'OFFLINE';
                }
            } catch (e) {
                console.error(e);
            }
        }
        setInterval(updateStats, 1000);
        updateStats();
    </script>
</body>
</html>)html";
    return QByteArray::fromRawData(html, sizeof(html) - 1);
}

}  // namespace tftp
