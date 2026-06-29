#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

#include "core/tftp_client.h"
#include "core/tftp_protocol.h"
#include "core/tftp_server.h"

using namespace tftp;

// ---------------------------------------------------------------------------
// AetherTFTP protocol & transfer test suite.
//
// Combines pure protocol (de)serialization checks with full client<->server
// transfers over the loopback interface, per the project test plan.
// ---------------------------------------------------------------------------
class TFTPProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    // Pure protocol layer.
    void testRequestRoundTrip();
    void testDataAckRoundTrip();
    void testErrorRoundTrip();
    void testOackRoundTrip();
    void testBlockSizeClamp();

    // End-to-end loopback transfers.
    void testDownloadDefaultBlockSize();
    void testBlockSizeNegotiation();
    void testUploadRoundTrip();
    void testUploadOverwriteRejected();
    void testLargeMultiBlockTransfer();
    void testMissingFileError();
    void testPathTraversalRejected();
    void testAcls();
    void testSymlinkPathTraversal();
    void testIpv6Binding();
    void testSinglePortMultiplexing();
    void testRateLimiting();
    void testStructuredLogging();
    void testPrometheusExporter();
    void testMetricsExporterHardening();

private:
    QByteArray makePayload(int size, char seed) const;
    QString writeServerFile(const QString &name, const QByteArray &data) const;

    TftpServer *m_server = nullptr;
    QTemporaryDir m_serverDir;  // files served by m_server.
    QTemporaryDir
        m_clientDir;  // destination for downloads / source for uploads.
    quint16 m_port = 0;
};

void TFTPProtocolTest::initTestCase() {
    QVERIFY(m_serverDir.isValid());
    QVERIFY(m_clientDir.isValid());
    m_server = new TftpServer(this);
    // Port 0 lets the OS pick a free port — avoids privileged-port and
    // collision problems in CI.
    QVERIFY(m_server->listen(QHostAddress(QStringLiteral("127.0.0.1")), 0,
                             m_serverDir.path()));
    m_port = m_server->port();
    QVERIFY(m_port != 0);
}

void TFTPProtocolTest::cleanupTestCase() {
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

QByteArray TFTPProtocolTest::makePayload(int size, char seed) const {
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = char((i * 31 + seed) & 0xFF);
    return data;
}

QString TFTPProtocolTest::writeServerFile(const QString &name,
                                          const QByteArray &data) const {
    const QString path = m_serverDir.path() + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(data);
    f.close();
    return path;
}

// Pure protocol tests
void TFTPProtocolTest::testRequestRoundTrip() {
    Options opts;
    opts.insert(QStringLiteral("blksize"), QStringLiteral("8192"));
    opts.insert(QStringLiteral("tsize"), QStringLiteral("0"));
    const QByteArray dg =
        buildRequest(OpCode::WRQ, QStringLiteral("firmware.bin"),
                     QStringLiteral("octet"), opts);

    Request req;
    QVERIFY(parseRequest(dg, req));
    QCOMPARE(req.op, OpCode::WRQ);
    QCOMPARE(req.filename, QStringLiteral("firmware.bin"));
    QCOMPARE(req.mode, QStringLiteral("octet"));
    QCOMPARE(req.options.value(QStringLiteral("blksize")),
             QStringLiteral("8192"));
    QCOMPARE(req.options.value(QStringLiteral("tsize")), QStringLiteral("0"));
}

void TFTPProtocolTest::testDataAckRoundTrip() {
    const QByteArray payload = makePayload(512, 7);
    const QByteArray dg = buildData(1234, payload);
    quint16 block = 0;
    QByteArray out;
    QVERIFY(parseData(dg, block, out));
    QCOMPARE(block, quint16(1234));
    QCOMPARE(out, payload);

    quint16 ackBlock = 0;
    QVERIFY(parseAck(buildAck(4321), ackBlock));
    QCOMPARE(ackBlock, quint16(4321));
}

void TFTPProtocolTest::testErrorRoundTrip() {
    const QByteArray dg =
        buildError(ErrorCode::FileNotFound, QStringLiteral("nope"));
    ErrorCode code;
    QString msg;
    QVERIFY(parseError(dg, code, msg));
    QCOMPARE(code, ErrorCode::FileNotFound);
    QCOMPARE(msg, QStringLiteral("nope"));
}

void TFTPProtocolTest::testOackRoundTrip() {
    Options opts;
    opts.insert(QStringLiteral("blksize"), QStringLiteral("1428"));
    const QByteArray dg = buildOack(opts);
    Options parsed;
    QVERIFY(parseOack(dg, parsed));
    QCOMPARE(parsed.value(QStringLiteral("blksize")), QStringLiteral("1428"));
}

void TFTPProtocolTest::testBlockSizeClamp() {
    QCOMPARE(clampBlockSize(1), kMinBlockSize);
    QCOMPARE(clampBlockSize(100000), kMaxBlockSize);
    QCOMPARE(clampBlockSize(8192), 8192);
}

// End-to-end loopback tests
void TFTPProtocolTest::testDownloadDefaultBlockSize() {
    const QByteArray data = makePayload(1000, 1);  // > 1 block at 512.
    QVERIFY(!writeServerFile(QStringLiteral("small.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/small.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("small.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    QCOMPARE(client.negotiatedBlockSize(), kDefaultBlockSize);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
}

void TFTPProtocolTest::testBlockSizeNegotiation() {
    const QByteArray data = makePayload(20000, 2);
    QVERIFY(!writeServerFile(QStringLiteral("test_image.bin"), data).isEmpty());

    TftpClient client;
    client.setBlockSize(8192);  // RFC 2348 option negotiation.
    const QString outPath = m_clientDir.path() + QStringLiteral("/out.bin");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("test_image.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    QCOMPARE(client.negotiatedBlockSize(), 8192);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
}

void TFTPProtocolTest::testUploadRoundTrip() {
    const QByteArray data = makePayload(5000, 3);
    const QString srcPath =
        m_clientDir.path() + QStringLiteral("/upload_src.bin");
    QFile src(srcPath);
    QVERIFY(src.open(QIODevice::WriteOnly));
    src.write(data);
    src.close();

    TftpClient client;
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath,
                      QStringLiteral("uploaded.bin"));
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile dst(m_serverDir.path() + QStringLiteral("/uploaded.bin"));
    QVERIFY(dst.open(QIODevice::ReadOnly));
    QCOMPARE(dst.readAll(), data);
}

void TFTPProtocolTest::testUploadOverwriteRejected() {
    // First upload succeeds; a second upload of the same remote name must be
    // rejected (RFC 1350 — server replies FileAlreadyExists).
    const QByteArray data = makePayload(1024, 5);
    const QString srcPath = m_clientDir.path() + QStringLiteral("/ow_src.bin");
    QFile src(srcPath);
    QVERIFY(src.open(QIODevice::WriteOnly));
    src.write(data);
    src.close();

    TftpClient first;
    QSignalSpy firstSpy(&first, &TftpClient::transferFinished);
    first.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath,
                     QStringLiteral("overwrite.bin"));
    QVERIFY(firstSpy.wait(5000));
    QCOMPARE(firstSpy.takeFirst().at(0).toBool(), true);

    TftpClient second;
    QSignalSpy secondSpy(&second, &TftpClient::transferFinished);
    second.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath,
                      QStringLiteral("overwrite.bin"));
    QVERIFY(secondSpy.wait(5000));
    QCOMPARE(secondSpy.takeFirst().at(0).toBool(), false);
}

void TFTPProtocolTest::testLargeMultiBlockTransfer() {
    // Exercise an exact multiple of the block size (needs a final empty block).
    const QByteArray data = makePayload(512 * 4, 9);
    QVERIFY(!writeServerFile(QStringLiteral("exact.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/exact.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("exact.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll().size(), data.size());
    QCOMPARE(out.readAll(), QByteArray());  // already at end
}

void TFTPProtocolTest::testMissingFileError() {
    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/missing.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("does_not_exist.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

void TFTPProtocolTest::testPathTraversalRejected() {
    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/escape.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("../../../etc/passwd"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

void TFTPProtocolTest::testAcls() {
    // Read-only mode
    m_server->setReadOnly(true);
    // Download should still work
    {
        const QByteArray data = makePayload(100, 10);
        QVERIFY(!writeServerFile(QStringLiteral("acl_ro.bin"), data).isEmpty());
        TftpClient client;
        const QString outPath =
            m_clientDir.path() + QStringLiteral("/acl_ro.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                            QStringLiteral("acl_ro.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    }
    // Upload should fail
    {
        const QString srcPath =
            m_clientDir.path() + QStringLiteral("/acl_upload_src.bin");
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("test");
        src.close();
        TftpClient client;
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath,
                          QStringLiteral("acl_ro_upload.bin"));
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }
    m_server->setReadOnly(false);

    // Blacklisting
    m_server->setBlacklist({QStringLiteral("127.0.0.1/32")});
    {
        TftpClient client;
        const QString outPath =
            m_clientDir.path() + QStringLiteral("/acl_black.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                            QStringLiteral("acl_ro.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }
    m_server->setBlacklist({});

    // Whitelisting
    m_server->setWhitelist(
        {QStringLiteral("10.0.0.0/8")});  // does not include 127.0.0.1
    {
        TftpClient client;
        const QString outPath =
            m_clientDir.path() + QStringLiteral("/acl_white.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                            QStringLiteral("acl_ro.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }
    m_server->setWhitelist({});
}

void TFTPProtocolTest::testSymlinkPathTraversal() {
    // Create a file outside the server root.
    QTemporaryDir outsideDir;
    QVERIFY(outsideDir.isValid());
    QFile outsideFile(outsideDir.path() + QStringLiteral("/secret.txt"));
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    outsideFile.write("classified information");
    outsideFile.close();

    // Create a symlink inside the server root pointing to the outside file.
    QString symlinkPath =
        m_serverDir.path() + QStringLiteral("/link_to_secret.txt");
#ifdef Q_OS_WIN
    if (!QFile::link(outsideFile.fileName(), symlinkPath)) {
        QSKIP(
            "Symlink creation is not permitted or supported on this Windows "
            "host (requires developer mode / admin privileges).");
    }
#else
    QVERIFY(QFile::link(outsideFile.fileName(), symlinkPath));
#endif

    // A download request for the symlink name should be rejected by path
    // traversal protection.
    TftpClient client;
    const QString outPath =
        m_clientDir.path() + QStringLiteral("/symlink_traversal.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("link_to_secret.txt"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);  // Must fail!
}

void TFTPProtocolTest::testIpv6Binding() {
#ifdef Q_OS_WIN
    QSKIP(
        "IPv6 loopback testing is skipped on Windows due to CI virtual "
        "environment network routing limitations.");
#else
    TftpServer ipv6Server;
    // Bind to LocalHostIPv6 (::1).
    if (!ipv6Server.listen(QHostAddress::LocalHostIPv6, 0,
                           m_serverDir.path())) {
        QSKIP("IPv6 is not supported/configured on this loopback interface");
    }
    quint16 ipv6Port = ipv6Server.port();

    const QByteArray data = makePayload(200, 11);
    QVERIFY(!writeServerFile(QStringLiteral("ipv6.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/ipv6.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("::1"), ipv6Port,
                        QStringLiteral("ipv6.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
#endif
}

void TFTPProtocolTest::testSinglePortMultiplexing() {
    m_server->setSinglePortMode(true);

    const QByteArray data = makePayload(1500, 12);  // multiple blocks
    QVERIFY(!writeServerFile(QStringLiteral("singleport.bin"), data).isEmpty());

    TftpClient client;
    client.setBlockSize(512);
    const QString outPath =
        m_clientDir.path() + QStringLiteral("/singleport.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("singleport.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);

    m_server->setSinglePortMode(false);
}

void TFTPProtocolTest::testRateLimiting() {
    // Write a test file of 2000 bytes
    const QByteArray data = makePayload(2000, 42);
    QVERIFY(!writeServerFile(QStringLiteral("ratelimit.bin"), data).isEmpty());

    // Session speed limit: 1000 bytes/sec.
    // 2000 bytes should take around 1 second (>= 800 ms with burst allowance)
    m_server->setSessionLimit(1000);
    m_server->setGlobalLimit(0);

    QElapsedTimer timer;
    timer.start();

    TftpClient client;
    client.setBlockSize(512);
    const QString outPath1 =
        m_clientDir.path() + QStringLiteral("/ratelimit_sess.out");
    QSignalSpy spy1(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("ratelimit.bin"), outPath1);
    QVERIFY(spy1.wait(5000));
    QCOMPARE(spy1.takeFirst().at(0).toBool(), true);

    qint64 elapsedSess = timer.elapsed();
    QVERIFY2(
        elapsedSess >= 800,
        qPrintable(QString("Session rate limit took %1 ms, expected >= 800 ms")
                       .arg(elapsedSess)));

    // Clean up file
    QFile::remove(outPath1);

    // Wait for server-side session to clean up completely
    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }

    // Global speed limit: 500 bytes/sec.
    // 2000 bytes should take around 2 seconds (>= 1500 ms)
    m_server->setSessionLimit(0);
    m_server->setGlobalLimit(500);

    timer.restart();

    const QString outPath2 =
        m_clientDir.path() + QStringLiteral("/ratelimit_glob.out");
    QSignalSpy spy2(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("ratelimit.bin"), outPath2);
    QVERIFY(spy2.wait(5000));
    QCOMPARE(spy2.takeFirst().at(0).toBool(), true);

    qint64 elapsedGlob = timer.elapsed();
    QVERIFY2(
        elapsedGlob >= 1500,
        qPrintable(QString("Global rate limit took %1 ms, expected >= 1500 ms")
                       .arg(elapsedGlob)));

    // Clean up
    QFile::remove(outPath2);
    m_server->setSessionLimit(0);
    m_server->setGlobalLimit(0);

    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }
}

void TFTPProtocolTest::testStructuredLogging() {
    m_server->setJsonLoggingEnabled(true);

    QList<QString> jsonLogs;
    auto conn = connect(m_server, &TftpServer::logMessage, this,
                        [&jsonLogs](const QString &msg) {
                            if (msg.startsWith('{')) {
                                jsonLogs.append(msg);
                            }
                        });

    const QByteArray data = makePayload(1000, 43);
    QVERIFY(!writeServerFile(QStringLiteral("jsonlog.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/jsonlog.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("jsonlog.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    // Wait for server to finish logging before disconnecting and asserting
    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }

    disconnect(conn);
    m_server->setJsonLoggingEnabled(false);

    QFile::remove(outPath);

    QVERIFY(!jsonLogs.isEmpty());

    bool foundStart = false;
    bool foundComplete = false;

    for (const QString &logStr : jsonLogs) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(logStr.toUtf8(), &err);
        QCOMPARE(err.error, QJsonParseError::NoError);
        QVERIFY(doc.isObject());

        QJsonObject obj = doc.object();
        QVERIFY(obj.contains(QStringLiteral("timestamp")));
        QVERIFY(obj.contains(QStringLiteral("event_type")));
        QVERIFY(obj.contains(QStringLiteral("session_id")));
        QVERIFY(obj.contains(QStringLiteral("client_ip")));
        QVERIFY(obj.contains(QStringLiteral("file_name")));
        QVERIFY(obj.contains(QStringLiteral("block_count")));
        QVERIFY(obj.contains(QStringLiteral("status")));

        QString evType = obj.value(QStringLiteral("event_type")).toString();
        if (evType == QLatin1String("transfer_start")) {
            foundStart = true;
            QCOMPARE(obj.value(QStringLiteral("status")).toString(),
                     QStringLiteral("started"));
        } else if (evType == QLatin1String("transfer_complete")) {
            foundComplete = true;
            QCOMPARE(obj.value(QStringLiteral("status")).toString(),
                     QStringLiteral("success"));
            QCOMPARE(obj.value(QStringLiteral("block_count")).toInt(),
                     2);  // 1000 bytes with 512 block size is 2 blocks
        }
    }

    QVERIFY(foundStart);
    QVERIFY(foundComplete);
}

void TFTPProtocolTest::testPrometheusExporter() {
    // Start metrics exporter on dynamic port
    QVERIFY(m_server->startMetricsServer(0));
    quint16 mPort = m_server->metricsServerPort();
    QVERIFY(mPort > 0);

    const QByteArray data = makePayload(2000, 44);
    QVERIFY(!writeServerFile(QStringLiteral("metrics.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/metrics.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port,
                        QStringLiteral("metrics.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile::remove(outPath);

    // Wait for server session cleanup so active sessions counts go to 0 and
    // totals are recorded
    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }

    // Query metrics exporter using QTcpSocket
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, mPort);
    QVERIFY(socket.waitForConnected(2000));

    // Send HTTP GET request and flush
    socket.write("GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    socket.flush();

    // Use a QEventLoop to wait for readyRead asynchronously so the server side
    // gets events processed
    QEventLoop loop;
    connect(&socket, &QTcpSocket::readyRead, &loop, &QEventLoop::quit);
    connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray response = socket.readAll();
    while (socket.state() == QAbstractSocket::ConnectedState &&
           socket.waitForReadyRead(100)) {
        response.append(socket.readAll());
    }

    socket.close();

    QString respStr = QString::fromUtf8(response);
    QVERIFY(respStr.contains(QStringLiteral("HTTP/1.1 200 OK")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_active_sessions")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_bytes_transferred_total")));
    QVERIFY(respStr.contains(
        QStringLiteral("tftp_transfers_total{status=\"success\"}")));
    QVERIFY(respStr.contains(
        QStringLiteral("tftp_transfers_total{status=\"failure\"}")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_retransmissions_total")));

    m_server->stopMetricsServer();
}

void TFTPProtocolTest::testMetricsExporterHardening() {
    QVERIFY(m_server->startMetricsServer(0));
    const quint16 mPort = m_server->metricsServerPort();
    QVERIFY(mPort > 0);

    // Issue a raw HTTP request and return the full response text. Uses a
    // QEventLoop so the in-process exporter gets its events serviced.
    auto request = [mPort](const QByteArray &raw) -> QString {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, mPort);
        if (!socket.waitForConnected(2000))
            return QString();
        socket.write(raw);
        socket.flush();

        QEventLoop loop;
        QObject::connect(&socket, &QTcpSocket::readyRead, &loop,
                         &QEventLoop::quit);
        QObject::connect(&socket, &QTcpSocket::disconnected, &loop,
                         &QEventLoop::quit);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();

        QByteArray response = socket.readAll();
        while (socket.state() == QAbstractSocket::ConnectedState &&
               socket.waitForReadyRead(100)) {
            response.append(socket.readAll());
        }
        socket.close();
        return QString::fromUtf8(response);
    };

    // Non-GET method is rejected with 405 (and not served metrics).
    const QString post =
        request("POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY2(post.contains(QStringLiteral("405")), qPrintable(post));
    QVERIFY(!post.contains(QStringLiteral("tftp_active_sessions")));

    // Unknown paths return 404.
    const QString notFound =
        request("GET /not-here HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY2(notFound.contains(QStringLiteral("404")), qPrintable(notFound));

    // A query string on /metrics is tolerated and still served.
    const QString ok =
        request("GET /metrics?foo=bar HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY2(ok.contains(QStringLiteral("200 OK")), qPrintable(ok));
    QVERIFY(ok.contains(QStringLiteral("tftp_active_sessions")));

    m_server->stopMetricsServer();
}

QTEST_MAIN(TFTPProtocolTest)
#include "tftp_test.moc"
