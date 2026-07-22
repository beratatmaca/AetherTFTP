#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

namespace tftp {

class TftpServer;

/**
 * @brief Embedded HTTP Web Server providing a real-time web dashboard & REST API.
 */
class EmbeddedWebServer : public QObject {
    Q_OBJECT
public:
    explicit EmbeddedWebServer(TftpServer *tftpServer, QObject *parent = nullptr);
    ~EmbeddedWebServer() override;

    bool listen(const QHostAddress &address, quint16 port);
    void close();
    bool isListening() const;
    quint16 port() const;

private slots:
    void onNewConnection();
    void onClientReadyRead();

private:
    void handleRequest(QTcpSocket *socket, const QString &method, const QString &path, const QByteArray &body);
    static void sendResponse(QTcpSocket *socket, int statusCode, const QString &statusText, const QString &contentType,
                             const QByteArray &body);
    static QByteArray getEmbeddedHtmlPage();

    TftpServer *m_tftpServer = nullptr;
    QTcpServer *m_tcpServer = nullptr;
};

}  // namespace tftp
